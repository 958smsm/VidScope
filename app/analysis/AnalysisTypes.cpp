#include "analysis/AnalysisTypes.h"

#include <algorithm>
#include <limits>
#include <mutex>

namespace vidscope::analysis {
namespace {

[[nodiscard]] media::MediaTime absoluteDistance(
    const media::MediaTime left,
    const media::MediaTime right) noexcept
{
    if (left >= right) {
        return left - right;
    }
    return right - left;
}

} // namespace

AnalysisStore::AnalysisStore(const std::size_t maximumSamples)
    : maximumSamples_(maximumSamples)
{
    samples_.reserve(std::min<std::size_t>(maximumSamples_, 65'536));
}

bool AnalysisStore::orderedBefore(
    const AnalysisSample& left,
    const AnalysisSample& right) noexcept
{
    if (left.presentationTime != right.presentationTime) {
        return left.presentationTime < right.presentationTime;
    }
    if (left.presentationIndex >= 0 && right.presentationIndex >= 0
        && left.presentationIndex != right.presentationIndex) {
        return left.presentationIndex < right.presentationIndex;
    }
    if (left.pts != right.pts) {
        return left.pts < right.pts;
    }
    return left.presentationIndex < right.presentationIndex;
}

bool AnalysisStore::sameIdentity(
    const AnalysisSample& left,
    const AnalysisSample& right) noexcept
{
    if (left.presentationIndex >= 0 && right.presentationIndex >= 0) {
        return left.presentationIndex == right.presentationIndex;
    }
    return left.presentationTime == right.presentationTime && left.pts == right.pts;
}

bool AnalysisStore::upsert(AnalysisSample sample)
{
    std::unique_lock lock(mutex_);
    if (!samples_.empty() && sample.presentationIndex >= 0
        && samples_.back().presentationIndex >= 0
        && sample.presentationIndex > samples_.back().presentationIndex
        && !orderedBefore(sample, samples_.back())) {
        if (samples_.size() >= maximumSamples_) {
            return false;
        }
        samples_.push_back(std::move(sample));
        return true;
    }
    auto position = std::lower_bound(samples_.begin(), samples_.end(), sample, orderedBefore);

    auto findIdentity = [&](auto begin, auto end) {
        return std::find_if(begin, end, [&](const AnalysisSample& existing) {
            return sameIdentity(existing, sample);
        });
    };

    auto existing = position;
    if (sample.presentationIndex >= 0) {
        existing = findIdentity(samples_.begin(), samples_.end());
    } else {
        auto firstAtTime = std::lower_bound(
            samples_.begin(),
            samples_.end(),
            sample.presentationTime,
            [](const AnalysisSample& candidate, const media::MediaTime time) {
                return candidate.presentationTime < time;
            });
        auto pastTime = std::upper_bound(
            firstAtTime,
            samples_.end(),
            sample.presentationTime,
            [](const media::MediaTime time, const AnalysisSample& candidate) {
                return time < candidate.presentationTime;
            });
        existing = findIdentity(firstAtTime, pastTime);
    }

    if (existing != samples_.end()) {
        const bool orderChanges = existing->presentationTime != sample.presentationTime
            || existing->presentationIndex != sample.presentationIndex
            || existing->pts != sample.pts;
        if (!orderChanges) {
            *existing = std::move(sample);
            return true;
        }
        samples_.erase(existing);
        position = std::lower_bound(samples_.begin(), samples_.end(), sample, orderedBefore);
        samples_.insert(position, std::move(sample));
        return true;
    }

    if (samples_.size() >= maximumSamples_) {
        return false;
    }
    samples_.insert(position, std::move(sample));
    return true;
}

std::size_t AnalysisStore::merge(std::vector<AnalysisSample> samples)
{
    std::size_t accepted = 0;
    for (auto& sample : samples) {
        if (upsert(std::move(sample))) {
            ++accepted;
        }
    }
    return accepted;
}

void AnalysisStore::replace(std::vector<AnalysisSample> samples)
{
    std::sort(samples.begin(), samples.end(), orderedBefore);
    if (samples.size() > maximumSamples_) {
        samples.resize(maximumSamples_);
    }
    samples.erase(
        std::unique(samples.begin(), samples.end(), sameIdentity),
        samples.end());

    std::unique_lock lock(mutex_);
    samples_ = std::move(samples);
}

void AnalysisStore::clear()
{
    std::unique_lock lock(mutex_);
    samples_.clear();
}

std::optional<AnalysisSample> AnalysisStore::nearest(
    const media::MediaTime time,
    const std::int64_t presentationIndex,
    const media::MediaTime maximumDistance) const
{
    std::shared_lock lock(mutex_);
    if (samples_.empty()) {
        return std::nullopt;
    }

    auto after = std::lower_bound(
        samples_.begin(),
        samples_.end(),
        time,
        [](const AnalysisSample& sample, const media::MediaTime target) {
            return sample.presentationTime < target;
        });
    auto best = after;
    if (after == samples_.end()) {
        best = std::prev(samples_.end());
    } else if (after != samples_.begin()) {
        const auto before = std::prev(after);
        if (absoluteDistance(before->presentationTime, time)
            <= absoluteDistance(after->presentationTime, time)) {
            best = before;
        }
    }

    if (best != samples_.end()
        && absoluteDistance(best->presentationTime, time) <= maximumDistance
        && (presentationIndex < 0 || best->presentationIndex < 0
            || best->presentationIndex == presentationIndex)) {
        return *best;
    }
    if (presentationIndex >= 0) {
        const auto exact = std::find_if(samples_.begin(), samples_.end(), [&](const auto& sample) {
            return sample.presentationIndex == presentationIndex;
        });
        if (exact != samples_.end()) {
            return *exact;
        }
    }
    if (best == samples_.end()
        || absoluteDistance(best->presentationTime, time) > maximumDistance) {
        return std::nullopt;
    }
    return *best;
}

std::vector<AnalysisSample> AnalysisStore::range(
    media::MediaTime start,
    media::MediaTime end,
    const std::size_t maximumResults) const
{
    if (end < start) {
        std::swap(start, end);
    }
    std::shared_lock lock(mutex_);
    const auto first = std::lower_bound(
        samples_.begin(),
        samples_.end(),
        start,
        [](const AnalysisSample& sample, const media::MediaTime target) {
            return sample.presentationTime < target;
        });
    const auto last = std::upper_bound(
        first,
        samples_.end(),
        end,
        [](const media::MediaTime target, const AnalysisSample& sample) {
            return target < sample.presentationTime;
        });
    const auto available = static_cast<std::size_t>(std::distance(first, last));
    const auto count = std::min(available, maximumResults);
    return {first, std::next(first, static_cast<std::ptrdiff_t>(count))};
}

std::vector<AnalysisSample> AnalysisStore::snapshot() const
{
    std::shared_lock lock(mutex_);
    return samples_;
}

std::optional<media::MediaTime> AnalysisStore::latestPresentationEnd() const noexcept
{
    std::shared_lock lock(mutex_);
    if (samples_.empty()) {
        return std::nullopt;
    }
    const auto& latest = samples_.back();
    return latest.presentationTime + latest.duration;
}

std::size_t AnalysisStore::size() const noexcept
{
    std::shared_lock lock(mutex_);
    return samples_.size();
}

std::size_t AnalysisStore::capacity() const noexcept
{
    return maximumSamples_;
}

} // namespace vidscope::analysis
