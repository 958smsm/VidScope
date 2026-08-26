#include "timeline/TimelineModel.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <tuple>
#include <utility>

extern "C" {
#include <libavutil/mathematics.h>
}

namespace vidscope::timeline {
namespace {

using TimeRep = media::MediaTime::rep;

constexpr TimeRep kNanosecondsPerSecond = 1'000'000'000;

[[nodiscard]] TimeRep clampRounded(const long double value) noexcept
{
    constexpr auto minimum = static_cast<long double>(std::numeric_limits<TimeRep>::min());
    constexpr auto maximum = static_cast<long double>(std::numeric_limits<TimeRep>::max());

    if (!std::isfinite(value)) {
        return value > 0.0L ? std::numeric_limits<TimeRep>::max()
                            : std::numeric_limits<TimeRep>::min();
    }
    if (value <= minimum) {
        return std::numeric_limits<TimeRep>::min();
    }
    if (value >= maximum) {
        return std::numeric_limits<TimeRep>::max();
    }
    return static_cast<TimeRep>(std::round(value));
}

[[nodiscard]] TimeRep clampCeiling(const long double value) noexcept
{
    constexpr auto maximum = static_cast<long double>(std::numeric_limits<TimeRep>::max());
    if (!std::isfinite(value) || value >= maximum) {
        return std::numeric_limits<TimeRep>::max();
    }
    if (value <= 1.0L) {
        return 1;
    }
    return static_cast<TimeRep>(std::ceil(value));
}

[[nodiscard]] bool frameIdLess(
    const media::FrameId& left,
    const media::FrameId& right) noexcept
{
    return std::tie(left.presentationIndex, left.pts, left.sessionSerial)
        < std::tie(right.presentationIndex, right.pts, right.sessionSerial);
}

[[nodiscard]] bool frameBoundaryLess(
    const FrameBoundary& left,
    const FrameBoundary& right) noexcept
{
    if (left.time != right.time) {
        return left.time < right.time;
    }
    return frameIdLess(left.id, right.id);
}

[[nodiscard]] bool logicalFrameIdEqual(
    const media::FrameId& left,
    const media::FrameId& right) noexcept
{
    if (left.presentationIndex >= 0 && right.presentationIndex >= 0) {
        return left.presentationIndex == right.presentationIndex;
    }
    return left == right;
}

[[nodiscard]] bool markerLess(
    const TimelineMarker& left,
    const TimelineMarker& right) noexcept
{
    if (left.time != right.time) {
        return left.time < right.time;
    }
    return left.id < right.id;
}

[[nodiscard]] TimeRep niceIntervalAtLeast(const long double requested) noexcept
{
    const TimeRep required = clampCeiling(requested);
    constexpr TimeRep maximum = std::numeric_limits<TimeRep>::max();

    TimeRep decade = 1;
    while (decade <= maximum / 10 && required > decade * 10) {
        decade *= 10;
    }

    if (required <= decade) {
        return decade;
    }
    if (decade <= maximum / 2 && required <= decade * 2) {
        return decade * 2;
    }
    if (decade <= maximum / 5 && required <= decade * 5) {
        return decade * 5;
    }
    if (decade <= maximum / 10) {
        return decade * 10;
    }
    return maximum;
}

[[nodiscard]] TimeRep nextNiceInterval(const TimeRep current) noexcept
{
    constexpr TimeRep maximum = std::numeric_limits<TimeRep>::max();
    if (current <= 0 || current == maximum) {
        return maximum;
    }

    TimeRep decade = 1;
    while (decade <= maximum / 10 && current >= decade * 10) {
        decade *= 10;
    }

    if (current < decade * 2 && decade <= maximum / 2) {
        return decade * 2;
    }
    if (current < decade * 5 && decade <= maximum / 5) {
        return decade * 5;
    }
    if (decade <= maximum / 10) {
        return decade * 10;
    }
    return maximum;
}

[[nodiscard]] std::optional<TimeRep> firstAlignedTick(
    const TimeRep start,
    const TimeRep interval) noexcept
{
    if (start < 0 || interval <= 0) {
        return std::nullopt;
    }

    const TimeRep remainder = start % interval;
    if (remainder == 0) {
        return start;
    }

    const TimeRep adjustment = interval - remainder;
    if (start > std::numeric_limits<TimeRep>::max() - adjustment) {
        return std::nullopt;
    }
    return start + adjustment;
}

[[nodiscard]] std::size_t alignedTickCount(
    const TimeRep start,
    const TimeRep end,
    const TimeRep interval) noexcept
{
    const auto first = firstAlignedTick(start, interval);
    if (!first || *first > end) {
        return 0;
    }

    const auto count = static_cast<std::uint64_t>((end - *first) / interval) + 1U;
    constexpr auto maximumSize = std::numeric_limits<std::size_t>::max();
    if (count > maximumSize) {
        return maximumSize;
    }
    return static_cast<std::size_t>(count);
}

} // namespace

FrameBoundaryView::FrameBoundaryView(
    const std::deque<FrameBoundary>& storage,
    const std::size_t offset,
    const std::size_t count) noexcept
    : storage_(&storage)
    , offset_(offset)
    , count_(count)
{
}

const std::deque<FrameBoundary>& FrameBoundaryView::emptyStorage() noexcept
{
    static const std::deque<FrameBoundary> empty;
    return empty;
}

FrameBoundaryView::const_iterator FrameBoundaryView::begin() const noexcept
{
    const auto& storage = storage_ ? *storage_ : emptyStorage();
    return storage.cbegin() + static_cast<std::ptrdiff_t>(offset_);
}

FrameBoundaryView::const_iterator FrameBoundaryView::end() const noexcept
{
    return begin() + static_cast<std::ptrdiff_t>(count_);
}

std::size_t FrameBoundaryView::size() const noexcept
{
    return count_;
}

bool FrameBoundaryView::empty() const noexcept
{
    return count_ == 0;
}

const FrameBoundary& FrameBoundaryView::operator[](const std::size_t index) const noexcept
{
    return *(begin() + static_cast<std::ptrdiff_t>(index));
}

const FrameBoundary& FrameBoundaryView::front() const noexcept
{
    return *begin();
}

const FrameBoundary& FrameBoundaryView::back() const noexcept
{
    return *(end() - 1);
}

TimelineModel::TimelineModel(
    const std::size_t maximumKnownFrames,
    const std::size_t maximumMarkers)
    : maximumKnownFrames_(maximumKnownFrames)
    , maximumMarkers_(maximumMarkers)
{
}

void TimelineModel::reset(media::MediaTime duration)
{
    if (duration == media::kNoMediaTime || duration < media::MediaTime::zero()) {
        duration = media::MediaTime::zero();
    }

    duration_ = duration;
    viewportStart_ = media::MediaTime::zero();
    viewportEnd_ = duration_;
    playhead_ = media::MediaTime::zero();
    knownFrames_.clear();
    presentationIndexLocations_.clear();
    markers_.clear();
    selection_.reset();
    selectionDetailsCache_.reset();
}

media::MediaTime TimelineModel::duration() const noexcept
{
    return duration_;
}

media::MediaTime TimelineModel::viewportStart() const noexcept
{
    return viewportStart_;
}

media::MediaTime TimelineModel::viewportEnd() const noexcept
{
    return viewportEnd_;
}

media::MediaTime TimelineModel::visibleDuration() const noexcept
{
    return viewportEnd_ >= viewportStart_
        ? viewportEnd_ - viewportStart_
        : media::MediaTime::zero();
}

media::MediaTime TimelineModel::playhead() const noexcept
{
    return playhead_;
}

bool TimelineModel::hasMedia() const noexcept
{
    return duration_ > media::MediaTime::zero();
}

bool TimelineModel::isShowingEntireMedia() const noexcept
{
    return hasMedia() && viewportStart_ == media::MediaTime::zero()
        && viewportEnd_ == duration_;
}

bool TimelineModel::setPlayhead(const media::MediaTime time) noexcept
{
    const auto clamped = clampTime(time);
    if (playhead_ == clamped) {
        return false;
    }
    playhead_ = clamped;
    return true;
}

bool TimelineModel::setViewport(media::MediaTime start, media::MediaTime end) noexcept
{
    if (!hasMedia()) {
        return false;
    }

    start = clampTime(start);
    end = clampTime(end);
    if (end < start) {
        std::swap(start, end);
    }

    const auto minimumDuration = std::min(kMinimumViewportDuration, duration_);
    if (end - start < minimumDuration) {
        const auto requestedDuration = end - start;
        const auto midpoint = start + requestedDuration / 2;
        auto expandedStart = midpoint >= minimumDuration / 2
            ? midpoint - minimumDuration / 2
            : media::MediaTime::zero();
        const auto maximumStart = duration_ - minimumDuration;
        expandedStart = std::min(expandedStart, maximumStart);
        start = expandedStart;
        end = start + minimumDuration;
    }

    if (viewportStart_ == start && viewportEnd_ == end) {
        return false;
    }
    viewportStart_ = start;
    viewportEnd_ = end;
    return true;
}

bool TimelineModel::showEntireMedia() noexcept
{
    if (!hasMedia() || isShowingEntireMedia()) {
        return false;
    }
    viewportStart_ = media::MediaTime::zero();
    viewportEnd_ = duration_;
    return true;
}

bool TimelineModel::zoomAt(const double factor, media::MediaTime anchor) noexcept
{
    if (!hasMedia() || !std::isfinite(factor) || factor <= 0.0
        || visibleDuration() <= media::MediaTime::zero()) {
        return false;
    }

    const TimeRep oldDuration = visibleDuration().count();
    const TimeRep minimumDuration = std::min(kMinimumViewportDuration, duration_).count();
    TimeRep newDuration = clampRounded(
        static_cast<long double>(oldDuration) / static_cast<long double>(factor));
    newDuration = std::clamp(newDuration, minimumDuration, duration_.count());
    if (newDuration == oldDuration) {
        return false;
    }

    anchor = std::clamp(clampTime(anchor), viewportStart_, viewportEnd_);
    const TimeRep oldOffset = (anchor - viewportStart_).count();
    const TimeRep newOffset = std::clamp(
        av_rescale_rnd(oldOffset, newDuration, oldDuration, AV_ROUND_NEAR_INF),
        TimeRep{0},
        newDuration);

    TimeRep newStart = anchor.count() >= newOffset ? anchor.count() - newOffset : 0;
    const TimeRep maximumStart = duration_.count() - newDuration;
    newStart = std::clamp(newStart, TimeRep{0}, maximumStart);

    viewportStart_ = media::MediaTime{newStart};
    viewportEnd_ = media::MediaTime{newStart + newDuration};
    return true;
}

bool TimelineModel::panBy(const media::MediaTime delta) noexcept
{
    if (!hasMedia() || delta == media::MediaTime::zero()
        || visibleDuration() >= duration_) {
        return false;
    }

    const TimeRep currentStart = viewportStart_.count();
    const TimeRep maximumStart = (duration_ - visibleDuration()).count();
    TimeRep newStart = currentStart;
    if (delta.count() > 0) {
        const TimeRep available = maximumStart - currentStart;
        newStart = delta.count() >= available ? maximumStart : currentStart + delta.count();
    } else {
        const TimeRep magnitude = delta.count() == std::numeric_limits<TimeRep>::min()
            ? std::numeric_limits<TimeRep>::max()
            : -delta.count();
        newStart = magnitude >= currentStart ? 0 : currentStart - magnitude;
    }

    if (newStart == currentStart) {
        return false;
    }
    const auto length = visibleDuration();
    viewportStart_ = media::MediaTime{newStart};
    viewportEnd_ = viewportStart_ + length;
    return true;
}

bool TimelineModel::panByPixels(
    const double pixelsTowardLaterTime,
    const double pixelWidth) noexcept
{
    if (!std::isfinite(pixelsTowardLaterTime) || !std::isfinite(pixelWidth)
        || pixelWidth <= 0.0 || pixelsTowardLaterTime == 0.0
        || visibleDuration() <= media::MediaTime::zero()) {
        return false;
    }

    const long double delta = static_cast<long double>(visibleDuration().count())
        * static_cast<long double>(pixelsTowardLaterTime)
        / static_cast<long double>(pixelWidth);
    return panBy(media::MediaTime{clampRounded(delta)});
}

double TimelineModel::timeToPixel(
    const media::MediaTime time,
    const double pixelLeft,
    const double pixelWidth) const noexcept
{
    if (!std::isfinite(pixelLeft)) {
        return 0.0;
    }
    if (!hasMedia() || !std::isfinite(pixelWidth) || pixelWidth <= 0.0
        || visibleDuration() <= media::MediaTime::zero()) {
        return pixelLeft;
    }

    const auto boundedTime = clampTime(time);
    const long double offset = static_cast<long double>(
        (boundedTime - viewportStart_).count());
    const long double coordinate = static_cast<long double>(pixelLeft)
        + offset * static_cast<long double>(pixelWidth)
            / static_cast<long double>(visibleDuration().count());
    if (!std::isfinite(coordinate)) {
        return coordinate > 0.0L ? std::numeric_limits<double>::max()
                                 : -std::numeric_limits<double>::max();
    }
    return static_cast<double>(coordinate);
}

media::MediaTime TimelineModel::pixelToTime(
    const double pixel,
    const double pixelLeft,
    const double pixelWidth) const noexcept
{
    if (!hasMedia() || !std::isfinite(pixelLeft) || !std::isfinite(pixelWidth)
        || pixelWidth <= 0.0 || visibleDuration() <= media::MediaTime::zero()
        || std::isnan(pixel)) {
        return viewportStart_;
    }

    long double relative = 0.0L;
    if (pixel == std::numeric_limits<double>::infinity()) {
        relative = 1.0L;
    } else if (pixel != -std::numeric_limits<double>::infinity()) {
        relative = (static_cast<long double>(pixel) - static_cast<long double>(pixelLeft))
            / static_cast<long double>(pixelWidth);
        relative = std::clamp(relative, 0.0L, 1.0L);
    }

    const TimeRep offset = std::clamp(
        clampRounded(relative * static_cast<long double>(visibleDuration().count())),
        TimeRep{0},
        visibleDuration().count());
    return viewportStart_ + media::MediaTime{offset};
}

std::optional<double> TimelineModel::frameToPixel(
    const media::FrameId& frame,
    const double pixelLeft,
    const double pixelWidth) const noexcept
{
    if (frame.presentationIndex >= 0) {
        const auto location = presentationIndexLocations_.find(frame.presentationIndex);
        if (location == presentationIndexLocations_.cend()) {
            return std::nullopt;
        }
        return timeToPixel(location->second.time, pixelLeft, pixelWidth);
    }

    const auto found = std::find_if(
        knownFrames_.cbegin(),
        knownFrames_.cend(),
        [&frame](const FrameBoundary& candidate) { return candidate.id == frame; });
    if (found == knownFrames_.cend()) {
        return std::nullopt;
    }
    return timeToPixel(found->time, pixelLeft, pixelWidth);
}

double TimelineModel::pixelsPerSecond(const double pixelWidth) const noexcept
{
    if (!std::isfinite(pixelWidth) || pixelWidth <= 0.0
        || visibleDuration() <= media::MediaTime::zero()) {
        return 0.0;
    }

    const long double value = static_cast<long double>(pixelWidth)
        * static_cast<long double>(kNanosecondsPerSecond)
        / static_cast<long double>(visibleDuration().count());
    if (value >= static_cast<long double>(std::numeric_limits<double>::max())) {
        return std::numeric_limits<double>::max();
    }
    return static_cast<double>(value);
}

media::MediaTime TimelineModel::timePerPixel(const double pixelWidth) const noexcept
{
    if (!std::isfinite(pixelWidth) || pixelWidth <= 0.0
        || visibleDuration() <= media::MediaTime::zero()) {
        return media::MediaTime::zero();
    }

    const long double value = static_cast<long double>(visibleDuration().count())
        / static_cast<long double>(pixelWidth);
    return media::MediaTime{std::max<TimeRep>(1, clampRounded(value))};
}

media::MediaTime TimelineModel::majorTickInterval(
    const double pixelWidth,
    const double minimumPixelSpacing) const noexcept
{
    if (!std::isfinite(pixelWidth) || pixelWidth <= 0.0
        || !std::isfinite(minimumPixelSpacing) || minimumPixelSpacing <= 0.0
        || visibleDuration() <= media::MediaTime::zero()) {
        return media::MediaTime::zero();
    }

    const long double requested = static_cast<long double>(visibleDuration().count())
        * static_cast<long double>(minimumPixelSpacing)
        / static_cast<long double>(pixelWidth);
    return media::MediaTime{niceIntervalAtLeast(requested)};
}

std::vector<media::MediaTime> TimelineModel::majorTicks(
    const double pixelWidth,
    const double minimumPixelSpacing,
    const std::size_t maximumTicks) const
{
    std::vector<media::MediaTime> result;
    if (maximumTicks == 0) {
        return result;
    }

    TimeRep interval = majorTickInterval(pixelWidth, minimumPixelSpacing).count();
    if (interval <= 0) {
        return result;
    }

    const TimeRep start = viewportStart_.count();
    const TimeRep end = viewportEnd_.count();
    while (alignedTickCount(start, end, interval) > maximumTicks) {
        const TimeRep next = nextNiceInterval(interval);
        if (next == interval) {
            break;
        }
        interval = next;
    }

    const auto first = firstAlignedTick(start, interval);
    if (!first || *first > end) {
        return result;
    }

    result.reserve(std::min(alignedTickCount(start, end, interval), maximumTicks));
    TimeRep tick = *first;
    while (tick <= end && result.size() < maximumTicks) {
        result.emplace_back(tick);
        if (tick > end - interval) {
            break;
        }
        tick += interval;
    }
    return result;
}

bool TimelineModel::observeFrame(const FrameBoundary& frame)
{
    if (!hasMedia() || maximumKnownFrames_ == 0 || frame.time == media::kNoMediaTime
        || frame.time < media::MediaTime::zero() || frame.time > duration_
        || frame.duration < media::MediaTime::zero()) {
        return false;
    }

    auto existing = knownFrames_.end();
    if (frame.id.presentationIndex >= 0) {
        const auto location = presentationIndexLocations_.find(frame.id.presentationIndex);
        if (location != presentationIndexLocations_.cend()) {
            const FrameBoundary located{
                location->second.id,
                location->second.time,
                media::MediaTime::zero(),
                false,
            };
            existing = std::lower_bound(
                knownFrames_.begin(), knownFrames_.end(), located, frameBoundaryLess);
            if (existing == knownFrames_.end()
                || existing->id != location->second.id
                || existing->time != location->second.time) {
                existing = std::find_if(
                    knownFrames_.begin(),
                    knownFrames_.end(),
                    [&frame](const FrameBoundary& candidate) {
                        return logicalFrameIdEqual(candidate.id, frame.id);
                    });
            }
        }
    } else {
        existing = std::find_if(
            knownFrames_.begin(),
            knownFrames_.end(),
            [&frame](const FrameBoundary& candidate) { return candidate.id == frame.id; });
    }

    if (existing != knownFrames_.end()) {
        if (*existing == frame) {
            return false;
        }
        if (existing->id.presentationIndex >= 0) {
            presentationIndexLocations_.erase(existing->id.presentationIndex);
        }
        knownFrames_.erase(existing);
    }

    if (knownFrames_.empty() || frameBoundaryLess(knownFrames_.back(), frame)) {
        knownFrames_.push_back(frame);
    } else if (frameBoundaryLess(frame, knownFrames_.front())) {
        knownFrames_.push_front(frame);
    } else {
        const auto insertion = std::lower_bound(
            knownFrames_.begin(), knownFrames_.end(), frame, frameBoundaryLess);
        knownFrames_.insert(insertion, frame);
    }

    if (frame.id.presentationIndex >= 0) {
        presentationIndexLocations_.insert_or_assign(
            frame.id.presentationIndex,
            PresentationIndexLocation{frame.time, frame.id});
    }
    selectionDetailsCache_.reset();
    enforceKnownFrameBound();

    if (frame.id.presentationIndex >= 0) {
        return presentationIndexLocations_.contains(frame.id.presentationIndex);
    }
    return std::any_of(
        knownFrames_.cbegin(),
        knownFrames_.cend(),
        [&frame](const FrameBoundary& candidate) {
            return logicalFrameIdEqual(candidate.id, frame.id);
        });
}

bool TimelineModel::observeFrame(const media::DecodedFrame& frame)
{
    return observeFrame(FrameBoundary{
        frame.id,
        frame.presentationTime,
        frame.duration,
        frame.keyFrame,
    });
}

std::size_t TimelineModel::knownFrameCount() const noexcept
{
    return knownFrames_.size();
}

std::size_t TimelineModel::maximumKnownFrames() const noexcept
{
    return maximumKnownFrames_;
}

FrameBoundaryView TimelineModel::knownFrames() const noexcept
{
    return FrameBoundaryView{knownFrames_, 0, knownFrames_.size()};
}

FrameBoundaryView TimelineModel::visibleFrameBoundaries(
    const double pixelWidth,
    const double minimumPixelSpacing,
    const std::size_t maximumTicks) const noexcept
{
    if (knownFrames_.empty() || maximumTicks == 0 || !std::isfinite(pixelWidth)
        || pixelWidth <= 0.0 || !std::isfinite(minimumPixelSpacing)
        || minimumPixelSpacing < 0.0) {
        return {};
    }

    const auto first = std::lower_bound(
        knownFrames_.cbegin(),
        knownFrames_.cend(),
        viewportStart_,
        [](const FrameBoundary& frame, const media::MediaTime time) {
            return frame.time < time;
        });
    const auto last = std::upper_bound(
        first,
        knownFrames_.cend(),
        viewportEnd_,
        [](const media::MediaTime time, const FrameBoundary& frame) {
            return time < frame.time;
        });

    const auto count = static_cast<std::size_t>(std::distance(first, last));
    if (count == 0 || count > maximumTicks) {
        return {};
    }

    if (minimumPixelSpacing > 0.0 && count > 1) {
        auto previous = first;
        for (auto current = std::next(first); current != last; ++current) {
            if (current->time != previous->time) {
                const double spacing = timeToPixel(current->time, 0.0, pixelWidth)
                    - timeToPixel(previous->time, 0.0, pixelWidth);
                if (spacing < minimumPixelSpacing) {
                    return {};
                }
            }
            previous = current;
        }
    }

    const auto offset = static_cast<std::size_t>(
        std::distance(knownFrames_.cbegin(), first));
    return FrameBoundaryView{knownFrames_, offset, count};
}

std::optional<std::uint64_t> TimelineModel::addMarker(
    const media::MediaTime time,
    const TimelineMarkerKind kind,
    QString label,
    QString category,
    QString note)
{
    if (!hasMedia() || markers_.size() >= maximumMarkers_ || nextMarkerId_ == 0) {
        return std::nullopt;
    }

    const std::uint64_t id = nextMarkerId_;
    nextMarkerId_ = id == std::numeric_limits<std::uint64_t>::max() ? 0 : id + 1;
    TimelineMarker marker{
        id,
        clampTime(time),
        kind,
        std::move(label),
        std::move(category),
        std::move(note)};
    const auto insertion = std::lower_bound(
        markers_.begin(), markers_.end(), marker, markerLess);
    markers_.insert(insertion, std::move(marker));
    return id;
}

bool TimelineModel::updateMarker(
    const std::uint64_t id,
    const media::MediaTime time,
    const TimelineMarkerKind kind,
    QString label,
    QString category,
    QString note)
{
    const auto found = std::find_if(
        markers_.begin(),
        markers_.end(),
        [id](const TimelineMarker& marker) { return marker.id == id; });
    if (found == markers_.end()) {
        return false;
    }

    TimelineMarker updated{
        id,
        clampTime(time),
        kind,
        std::move(label),
        std::move(category),
        std::move(note)};
    markers_.erase(found);
    const auto insertion = std::lower_bound(
        markers_.begin(), markers_.end(), updated, markerLess);
    markers_.insert(insertion, std::move(updated));
    return true;
}

bool TimelineModel::removeMarker(const std::uint64_t id)
{
    const auto found = std::find_if(
        markers_.begin(),
        markers_.end(),
        [id](const TimelineMarker& marker) { return marker.id == id; });
    if (found == markers_.end()) {
        return false;
    }
    markers_.erase(found);
    return true;
}

void TimelineModel::clearMarkers(const std::optional<TimelineMarkerKind> kind)
{
    if (!kind) {
        markers_.clear();
        return;
    }

    std::erase_if(markers_, [kind](const TimelineMarker& marker) {
        return marker.kind == *kind;
    });
}

std::span<const TimelineMarker> TimelineModel::markers() const noexcept
{
    return markers_;
}

std::span<const TimelineMarker> TimelineModel::visibleMarkers() const noexcept
{
    const auto first = std::lower_bound(
        markers_.cbegin(),
        markers_.cend(),
        viewportStart_,
        [](const TimelineMarker& marker, const media::MediaTime time) {
            return marker.time < time;
        });
    const auto last = std::upper_bound(
        first,
        markers_.cend(),
        viewportEnd_,
        [](const media::MediaTime time, const TimelineMarker& marker) {
            return time < marker.time;
        });
    const auto count = static_cast<std::size_t>(std::distance(first, last));
    if (count == 0) {
        return {};
    }
    const auto offset = static_cast<std::size_t>(std::distance(markers_.cbegin(), first));
    return std::span<const TimelineMarker>{markers_.data() + offset, count};
}

std::optional<media::MediaTime> TimelineModel::adjacentMarkerTime(
    const TimelineMarkerKind kind,
    const media::MediaTime reference,
    const bool forward) const noexcept
{
    if (forward) {
        const auto found = std::find_if(markers_.cbegin(), markers_.cend(), [&](const auto& marker) {
            return marker.kind == kind && marker.time > reference;
        });
        return found == markers_.cend()
            ? std::nullopt
            : std::optional<media::MediaTime>{found->time};
    }

    const auto found = std::find_if(markers_.crbegin(), markers_.crend(), [&](const auto& marker) {
        return marker.kind == kind && marker.time < reference;
    });
    return found == markers_.crend()
        ? std::nullopt
        : std::optional<media::MediaTime>{found->time};
}

bool TimelineModel::setSelection(
    media::MediaTime anchor,
    media::MediaTime extent) noexcept
{
    if (!hasMedia()) {
        return false;
    }
    anchor = clampTime(anchor);
    extent = clampTime(extent);
    TimelineSelection normalized{
        std::min(anchor, extent),
        std::max(anchor, extent),
    };
    if (selection_ && *selection_ == normalized) {
        return false;
    }
    selection_ = normalized;
    selectionDetailsCache_.reset();
    return true;
}

bool TimelineModel::setSelectionIn(const media::MediaTime time) noexcept
{
    return setSelection(time, selection_ ? selection_->end : time);
}

bool TimelineModel::setSelectionOut(const media::MediaTime time) noexcept
{
    return setSelection(selection_ ? selection_->start : time, time);
}

bool TimelineModel::clearSelection() noexcept
{
    if (!selection_) {
        return false;
    }
    selection_.reset();
    selectionDetailsCache_.reset();
    return true;
}

const std::optional<TimelineSelection>& TimelineModel::selection() const noexcept
{
    return selection_;
}

TimelineSelectionDetails TimelineModel::selectionDetails() const
{
    if (selectionDetailsCache_) {
        return *selectionDetailsCache_;
    }

    TimelineSelectionDetails details;
    if (!selection_) {
        selectionDetailsCache_ = details;
        return details;
    }
    details.range = *selection_;

    const auto first = std::lower_bound(
        knownFrames_.cbegin(),
        knownFrames_.cend(),
        selection_->start,
        [](const FrameBoundary& frame, const media::MediaTime time) {
            return frame.time < time;
        });
    const auto last = std::upper_bound(
        first,
        knownFrames_.cend(),
        selection_->end,
        [](const media::MediaTime time, const FrameBoundary& frame) {
            return time < frame.time;
        });

    details.knownFrameCount = static_cast<std::size_t>(std::distance(first, last));
    if (first == last) {
        selectionDetailsCache_ = details;
        return details;
    }

    details.firstFrame = first->id;
    const auto finalFrame = std::prev(last);
    details.lastFrame = finalFrame->id;

    bool contiguousPresentationIndices = first->id.presentationIndex >= 0;
    std::int64_t expectedIndex = first->id.presentationIndex;
    for (auto current = first; contiguousPresentationIndices && current != last; ++current) {
        if (current->id.presentationIndex != expectedIndex) {
            contiguousPresentationIndices = false;
            break;
        }
        if (std::next(current) != last) {
            if (expectedIndex == std::numeric_limits<std::int64_t>::max()) {
                contiguousPresentationIndices = false;
                break;
            }
            ++expectedIndex;
        }
    }

    if (contiguousPresentationIndices
        && first->time == selection_->start
        && finalFrame->time == selection_->end
        && details.knownFrameCount
            <= static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
        details.frameCount = static_cast<std::int64_t>(details.knownFrameCount);
    }
    selectionDetailsCache_ = details;
    return details;
}

media::MediaTime TimelineModel::clampTime(media::MediaTime time) const noexcept
{
    if (time == media::kNoMediaTime || time < media::MediaTime::zero()) {
        return media::MediaTime::zero();
    }
    return std::min(time, duration_);
}

void TimelineModel::enforceKnownFrameBound()
{
    while (knownFrames_.size() > maximumKnownFrames_) {
        const auto distanceFromPlayhead = [this](const FrameBoundary& frame) {
            return frame.time >= playhead_ ? frame.time - playhead_ : playhead_ - frame.time;
        };
        const auto frontDistance = distanceFromPlayhead(knownFrames_.front());
        const auto backDistance = distanceFromPlayhead(knownFrames_.back());
        if (frontDistance >= backDistance) {
            if (knownFrames_.front().id.presentationIndex >= 0) {
                presentationIndexLocations_.erase(
                    knownFrames_.front().id.presentationIndex);
            }
            knownFrames_.pop_front();
        } else {
            if (knownFrames_.back().id.presentationIndex >= 0) {
                presentationIndexLocations_.erase(
                    knownFrames_.back().id.presentationIndex);
            }
            knownFrames_.pop_back();
        }
    }
}

} // namespace vidscope::timeline
