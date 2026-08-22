#include "filmstrip/FilmstripModel.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace vidscope::filmstrip {
namespace {

using TimeRep = media::MediaTime::rep;
using namespace std::chrono_literals;

[[nodiscard]] std::uint64_t absoluteDistance(
    const media::MediaTime left,
    const media::MediaTime right) noexcept
{
    const auto leftCount = static_cast<std::uint64_t>(
        std::max<TimeRep>(0, left.count()));
    const auto rightCount = static_cast<std::uint64_t>(
        std::max<TimeRep>(0, right.count()));
    return leftCount >= rightCount
        ? leftCount - rightCount
        : rightCount - leftCount;
}

[[nodiscard]] media::MediaTime saturatingMultiply(
    const media::MediaTime value,
    const std::size_t multiplier) noexcept
{
    if (value <= media::MediaTime::zero() || multiplier == 0) {
        return media::MediaTime::zero();
    }

    const TimeRep count = value.count();
    const auto maximum = std::numeric_limits<TimeRep>::max();
    if (multiplier > static_cast<std::size_t>(maximum / count)) {
        return media::MediaTime::max();
    }
    return media::MediaTime(count * static_cast<TimeRep>(multiplier));
}

[[nodiscard]] media::MediaTime interpolatedTime(
    const media::MediaTime start,
    const media::MediaTime end,
    const std::size_t index,
    const std::size_t denominator) noexcept
{
    if (denominator == 0 || end <= start) {
        return start;
    }

    const TimeRep span = (end - start).count();
    const TimeRep divisor = static_cast<TimeRep>(denominator);
    const TimeRep quotient = span / divisor;
    const TimeRep remainder = span % divisor;
    const TimeRep indexValue = static_cast<TimeRep>(index);
    const TimeRep offset = quotient * indexValue
        + (remainder * indexValue) / divisor;
    return start + media::MediaTime(offset);
}

[[nodiscard]] std::optional<std::size_t> nearestKnownFrameIndex(
    const timeline::FrameBoundaryView frames,
    const media::MediaTime target) noexcept
{
    if (frames.empty()) {
        return std::nullopt;
    }

    const auto found = std::lower_bound(
        frames.begin(),
        frames.end(),
        target,
        [](const timeline::FrameBoundary& frame, const media::MediaTime time) {
            return frame.time < time;
        });
    if (found == frames.begin()) {
        return 0;
    }
    if (found == frames.end()) {
        return frames.size() - 1;
    }

    const auto previous = std::prev(found);
    return absoluteDistance(previous->time, target) <= absoluteDistance(found->time, target)
        ? static_cast<std::size_t>(std::distance(frames.begin(), previous))
        : static_cast<std::size_t>(std::distance(frames.begin(), found));
}

void attachKnownFrameHint(
    FilmstripTarget& target,
    const timeline::FrameBoundaryView frames) noexcept
{
    const auto nearest = nearestKnownFrameIndex(frames, target.requestedTime);
    if (!nearest) {
        return;
    }
    const auto& frame = frames[*nearest];
    target.presentationIndexHint = frame.id.presentationIndex;
    if (frame.time == target.requestedTime) {
        target.keyFrameHint = frame.keyFrame;
    }
}

void markNearestTargetCurrent(
    std::vector<FilmstripTarget>& targets,
    const media::MediaTime playhead) noexcept
{
    if (targets.empty()) {
        return;
    }

    auto nearest = targets.begin();
    auto nearestDistance = absoluteDistance(nearest->requestedTime, playhead);
    for (auto current = std::next(targets.begin()); current != targets.end(); ++current) {
        const auto distance = absoluteDistance(current->requestedTime, playhead);
        if (distance < nearestDistance) {
            nearest = current;
            nearestDistance = distance;
        }
    }
    nearest->current = true;
}

[[nodiscard]] std::vector<FilmstripTarget> distributedTargets(
    const media::MediaTime start,
    const media::MediaTime end,
    const std::size_t count,
    const timeline::FrameBoundaryView frames,
    const media::MediaTime playhead)
{
    std::vector<FilmstripTarget> targets;
    if (count == 0) {
        return targets;
    }

    if (count == 1 || end <= start) {
        FilmstripTarget target;
        target.requestedTime = end > start ? start + (end - start) / 2 : start;
        attachKnownFrameHint(target, frames);
        target.current = true;
        targets.push_back(std::move(target));
        return targets;
    }

    targets.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        FilmstripTarget target;
        target.requestedTime = interpolatedTime(start, end, index, count - 1);
        attachKnownFrameHint(target, frames);
        if (targets.empty() || targets.back().requestedTime != target.requestedTime) {
            targets.push_back(std::move(target));
        }
    }
    markNearestTargetCurrent(targets, playhead);
    return targets;
}

[[nodiscard]] bool hasContiguousPresentationIndices(
    const timeline::FrameBoundaryView frames,
    const std::size_t start,
    const std::size_t count) noexcept
{
    if (count == 0 || start > frames.size() || count > frames.size() - start) {
        return false;
    }

    const auto firstIndex = frames[start].id.presentationIndex;
    if (firstIndex < 0) {
        return false;
    }
    const auto serial = frames[start].id.sessionSerial;

    for (std::size_t offset = 1; offset < count; ++offset) {
        const auto& previous = frames[start + offset - 1];
        const auto& current = frames[start + offset];
        if (current.id.presentationIndex < 0
            || current.id.sessionSerial != serial
            || previous.id.presentationIndex == std::numeric_limits<std::int64_t>::max()
            || current.id.presentationIndex != previous.id.presentationIndex + 1
            || current.time <= previous.time) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<std::vector<FilmstripTarget>> contiguousFrameWindow(
    const timeline::FrameBoundaryView frames,
    const media::MediaTime playhead,
    const std::size_t count)
{
    if (count == 0 || frames.size() < count) {
        return std::nullopt;
    }

    const auto nearest = nearestKnownFrameIndex(frames, playhead);
    if (!nearest) {
        return std::nullopt;
    }

    std::size_t start = *nearest > count / 2 ? *nearest - count / 2 : 0;
    if (start + count > frames.size()) {
        start = frames.size() - count;
    }
    if (!hasContiguousPresentationIndices(frames, start, count)) {
        return std::nullopt;
    }

    std::vector<FilmstripTarget> targets;
    targets.reserve(count);
    for (std::size_t offset = 0; offset < count; ++offset) {
        const auto& frame = frames[start + offset];
        FilmstripTarget target;
        target.requestedTime = frame.time;
        target.presentationIndexHint = frame.id.presentationIndex;
        target.keyFrameHint = frame.keyFrame;
        targets.push_back(std::move(target));
    }
    markNearestTargetCurrent(targets, playhead);
    return targets;
}

[[nodiscard]] media::MediaTime localFrameSpacing(
    const timeline::FrameBoundaryView frames,
    const media::MediaTime playhead)
{
    const auto nearest = nearestKnownFrameIndex(frames, playhead);
    if (nearest && frames[*nearest].duration > media::MediaTime::zero()) {
        return frames[*nearest].duration;
    }

    std::vector<TimeRep> durations;
    if (nearest) {
        constexpr std::size_t maximumSamples = 129;
        const std::size_t radius = maximumSamples / 2;
        const std::size_t start = *nearest > radius ? *nearest - radius : 0;
        const std::size_t end = std::min(frames.size(), start + maximumSamples);
        durations.reserve(end - start);
        for (std::size_t index = start; index < end; ++index) {
            if (frames[index].duration > media::MediaTime::zero()) {
                durations.push_back(frames[index].duration.count());
            }
        }
    }

    if (!durations.empty()) {
        const auto middle = durations.begin()
            + static_cast<std::ptrdiff_t>(durations.size() / 2);
        std::nth_element(durations.begin(), middle, durations.end());
        return media::MediaTime(*middle);
    }

    // This is only a timestamp-request spacing fallback. The worker still seeks
    // and returns the actual nearest presentation frame; no frame identity is
    // inferred from this interval.
    return 40ms;
}

[[nodiscard]] std::vector<FilmstripTarget> aroundPlayheadTargets(
    const timeline::TimelineModel& timeline,
    const std::size_t count,
    bool& usesExactContiguousFrames)
{
    const auto frames = timeline.knownFrames();
    const auto playhead = timeline.playhead();
    if (auto exact = contiguousFrameWindow(frames, playhead, count)) {
        usesExactContiguousFrames = true;
        return std::move(*exact);
    }

    usesExactContiguousFrames = false;
    if (count == 0) {
        return {};
    }
    if (count == 1) {
        FilmstripTarget target;
        target.requestedTime = playhead;
        attachKnownFrameHint(target, frames);
        target.current = true;
        return {std::move(target)};
    }

    const auto duration = timeline.duration();
    auto spacing = std::max(localFrameSpacing(frames, playhead), media::MediaTime(1));
    const auto desiredSpan = saturatingMultiply(spacing, count - 1);
    if (desiredSpan >= duration) {
        return distributedTargets(
            media::MediaTime::zero(),
            duration,
            count,
            frames,
            playhead);
    }

    const std::size_t centerIndex = count / 2;
    const auto before = saturatingMultiply(spacing, centerIndex);
    media::MediaTime start = playhead > before ? playhead - before : media::MediaTime::zero();
    if (start > duration - desiredSpan) {
        start = duration - desiredSpan;
    }

    std::vector<FilmstripTarget> targets;
    targets.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        FilmstripTarget target;
        target.requestedTime = start + saturatingMultiply(spacing, index);
        attachKnownFrameHint(target, frames);
        targets.push_back(std::move(target));
    }
    markNearestTargetCurrent(targets, playhead);
    return targets;
}

} // namespace

void FilmstripModel::setMode(const FilmstripMode mode) noexcept
{
    mode_ = mode;
}

FilmstripMode FilmstripModel::mode() const noexcept
{
    return mode_;
}

void FilmstripModel::setCount(const std::size_t count) noexcept
{
    count_ = std::clamp(count, kMinimumCount, kMaximumCount);
}

std::size_t FilmstripModel::count() const noexcept
{
    return count_;
}

FilmstripPlan FilmstripModel::makePlan(const timeline::TimelineModel& timeline) const
{
    FilmstripPlan plan;
    plan.mode = mode_;
    plan.requestedCount = count_;
    if (!timeline.hasMedia()) {
        return plan;
    }

    plan.status = FilmstripPlanStatus::Ready;
    plan.rangeStart = media::MediaTime::zero();
    plan.rangeEnd = timeline.duration();

    switch (mode_) {
    case FilmstripMode::EntireVideo:
        break;
    case FilmstripMode::AroundCurrentPosition:
        plan.targets = aroundPlayheadTargets(
            timeline,
            count_,
            plan.usesExactContiguousFrames);
        if (!plan.targets.empty()) {
            plan.rangeStart = plan.targets.front().requestedTime;
            plan.rangeEnd = plan.targets.back().requestedTime;
        }
        return plan;
    case FilmstripMode::VisibleTimeline:
        plan.rangeStart = timeline.viewportStart();
        plan.rangeEnd = timeline.viewportEnd();
        break;
    case FilmstripMode::SelectedRange:
        if (!timeline.selection()) {
            plan.status = FilmstripPlanStatus::SelectionRequired;
            plan.rangeStart = media::MediaTime::zero();
            plan.rangeEnd = media::MediaTime::zero();
            return plan;
        }
        plan.rangeStart = timeline.selection()->start;
        plan.rangeEnd = timeline.selection()->end;
        break;
    }

    plan.targets = distributedTargets(
        plan.rangeStart,
        plan.rangeEnd,
        count_,
        timeline.knownFrames(),
        timeline.playhead());
    return plan;
}

} // namespace vidscope::filmstrip
