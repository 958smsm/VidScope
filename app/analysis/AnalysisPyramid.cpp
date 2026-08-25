#include "analysis/AnalysisPyramid.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>

namespace vidscope::analysis {
namespace {

[[nodiscard]] std::int64_t ceilDivide(
    const std::int64_t numerator,
    const std::size_t denominator) noexcept
{
    if (numerator <= 0 || denominator == 0) {
        return 0;
    }
    const auto divisor = static_cast<std::uint64_t>(denominator);
    const auto value = static_cast<std::uint64_t>(numerator);
    return static_cast<std::int64_t>(value / divisor + (value % divisor != 0 ? 1U : 0U));
}

[[nodiscard]] std::size_t ceilDivide(
    const std::size_t numerator,
    const std::size_t denominator) noexcept
{
    return numerator / denominator + (numerator % denominator != 0 ? 1U : 0U);
}

} // namespace

AnalysisPyramid::AnalysisPyramid(AnalysisPyramidConfig config)
    : config_(std::move(config))
{
    config_.maximumBaseBuckets = std::max<std::size_t>(1, config_.maximumBaseBuckets);
    config_.groupingFactor = std::clamp<std::size_t>(config_.groupingFactor, 2, 16);
    config_.maximumLevels = std::max<std::size_t>(1, config_.maximumLevels);
}

void AnalysisPyramid::reset(
    const media::MediaTime duration,
    const std::size_t estimatedSamples)
{
    std::unique_lock lock(mutex_);
    duration_ = std::max(duration, media::MediaTime::zero());
    baseSpanNanoseconds_ = 0;
    levels_.clear();
    if (duration_ <= media::MediaTime::zero()) {
        return;
    }

    const std::size_t targetBaseBuckets = std::clamp<std::size_t>(
        estimatedSamples,
        1,
        config_.maximumBaseBuckets);
    baseSpanNanoseconds_ = std::max<std::int64_t>(
        1,
        ceilDivide(duration_.count(), targetBaseBuckets));
    std::size_t bucketCount = static_cast<std::size_t>(
        ceilDivide(duration_.count(), static_cast<std::size_t>(baseSpanNanoseconds_)));
    bucketCount = std::max<std::size_t>(1, bucketCount);
    levels_.emplace_back(bucketCount);

    while (bucketCount > 1 && levels_.size() < config_.maximumLevels) {
        bucketCount = ceilDivide(bucketCount, config_.groupingFactor);
        levels_.emplace_back(bucketCount);
    }
}

void AnalysisPyramid::clear()
{
    std::unique_lock lock(mutex_);
    duration_ = media::MediaTime::zero();
    baseSpanNanoseconds_ = 0;
    levels_.clear();
}

void AnalysisPyramid::accumulate(
    Accumulator& target,
    const AnalysisSample& sample) noexcept
{
    ++target.sampleCount;
    if (sample.motion) {
        const float value = std::clamp(*sample.motion, 0.0F, 1.0F);
        target.motionSum += value;
        target.minMotion = std::min(target.minMotion, value);
        target.maxMotion = std::max(target.maxMotion, value);
        ++target.motionCount;
    }
    if (sample.similarity) {
        const float value = std::clamp(*sample.similarity, 0.0F, 1.0F);
        target.similaritySum += value;
        target.minSimilarity = std::min(target.minSimilarity, value);
        target.maxSimilarity = std::max(target.maxSimilarity, value);
        ++target.similarityCount;
    }
}

void AnalysisPyramid::merge(Accumulator& target, const Accumulator& source) noexcept
{
    if (source.sampleCount == 0) {
        return;
    }
    target.sampleCount += source.sampleCount;
    if (source.motionCount > 0) {
        target.motionSum += source.motionSum;
        target.minMotion = std::min(target.minMotion, source.minMotion);
        target.maxMotion = std::max(target.maxMotion, source.maxMotion);
        target.motionCount += source.motionCount;
    }
    if (source.similarityCount > 0) {
        target.similaritySum += source.similaritySum;
        target.minSimilarity = std::min(target.minSimilarity, source.minSimilarity);
        target.maxSimilarity = std::max(target.maxSimilarity, source.maxSimilarity);
        target.similarityCount += source.similarityCount;
    }
}

std::int64_t AnalysisPyramid::levelSpan(const std::size_t level) const noexcept
{
    std::int64_t span = baseSpanNanoseconds_;
    for (std::size_t current = 0; current < level; ++current) {
        if (span > std::numeric_limits<std::int64_t>::max()
                / static_cast<std::int64_t>(config_.groupingFactor)) {
            return std::numeric_limits<std::int64_t>::max();
        }
        span *= static_cast<std::int64_t>(config_.groupingFactor);
    }
    return span;
}

std::size_t AnalysisPyramid::bucketIndex(
    media::MediaTime time,
    const std::size_t level) const noexcept
{
    if (levels_.empty() || level >= levels_.size() || levels_[level].empty()) {
        return 0;
    }
    time = std::clamp(time, media::MediaTime::zero(), duration_);
    if (time >= duration_) {
        return levels_[level].size() - 1;
    }
    const std::int64_t span = levelSpan(level);
    if (span <= 0) {
        return 0;
    }
    const auto index = static_cast<std::size_t>(time.count() / span);
    return std::min(index, levels_[level].size() - 1);
}

media::MediaTime AnalysisPyramid::bucketBoundary(
    const std::size_t index,
    const std::int64_t span) const noexcept
{
    if (span <= 0 || index == 0) {
        return media::MediaTime::zero();
    }
    const auto maximum = static_cast<std::uint64_t>(duration_.count());
    const auto spanValue = static_cast<std::uint64_t>(span);
    const auto indexValue = static_cast<std::uint64_t>(index);
    if (indexValue > maximum / spanValue) {
        return duration_;
    }
    return std::min(duration_, media::MediaTime(static_cast<std::int64_t>(indexValue * spanValue)));
}

void AnalysisPyramid::rebuild(std::span<const AnalysisSample> samples)
{
    std::unique_lock lock(mutex_);
    if (levels_.empty()) {
        return;
    }
    for (auto& level : levels_) {
        std::fill(level.begin(), level.end(), Accumulator{});
    }
    for (const auto& sample : samples) {
        if (sample.presentationTime == media::kNoMediaTime) {
            continue;
        }
        accumulate(levels_.front()[bucketIndex(sample.presentationTime, 0)], sample);
    }
    rebuildParents(0, levels_.front().size() - 1);
}

void AnalysisPyramid::rebuildParents(
    std::size_t firstBucket,
    std::size_t lastBucket)
{
    for (std::size_t level = 1; level < levels_.size(); ++level) {
        firstBucket /= config_.groupingFactor;
        lastBucket /= config_.groupingFactor;
        auto& parents = levels_[level];
        const auto& children = levels_[level - 1];
        lastBucket = std::min(lastBucket, parents.size() - 1);
        for (std::size_t parentIndex = firstBucket; parentIndex <= lastBucket; ++parentIndex) {
            Accumulator aggregate;
            const std::size_t childStart = parentIndex * config_.groupingFactor;
            const std::size_t childEnd = std::min(
                children.size(),
                childStart + config_.groupingFactor);
            for (std::size_t child = childStart; child < childEnd; ++child) {
                merge(aggregate, children[child]);
            }
            parents[parentIndex] = aggregate;
        }
    }
}

void AnalysisPyramid::replaceRange(
    media::MediaTime start,
    media::MediaTime end,
    const std::span<const AnalysisSample> samples)
{
    std::unique_lock lock(mutex_);
    if (levels_.empty()) {
        return;
    }
    start = std::clamp(start, media::MediaTime::zero(), duration_);
    end = std::clamp(end, media::MediaTime::zero(), duration_);
    if (end < start) {
        std::swap(start, end);
    }
    const std::size_t first = bucketIndex(start, 0);
    const std::size_t last = bucketIndex(end, 0);
    std::fill(
        levels_.front().begin() + static_cast<std::ptrdiff_t>(first),
        levels_.front().begin() + static_cast<std::ptrdiff_t>(last + 1),
        Accumulator{});
    for (const auto& sample : samples) {
        if (sample.presentationTime == media::kNoMediaTime) {
            continue;
        }
        const std::size_t index = bucketIndex(sample.presentationTime, 0);
        if (index >= first && index <= last) {
            accumulate(levels_.front()[index], sample);
        }
    }
    rebuildParents(first, last);
}

AnalysisPyramidRange AnalysisPyramid::alignedBaseRange(
    media::MediaTime start,
    media::MediaTime end) const noexcept
{
    std::shared_lock lock(mutex_);
    if (levels_.empty()) {
        return {};
    }
    start = std::clamp(start, media::MediaTime::zero(), duration_);
    end = std::clamp(end, media::MediaTime::zero(), duration_);
    if (end < start) {
        std::swap(start, end);
    }
    const std::size_t first = bucketIndex(start, 0);
    const std::size_t last = bucketIndex(end, 0);
    return {
        bucketBoundary(first, baseSpanNanoseconds_),
        bucketBoundary(last + 1, baseSpanNanoseconds_),
    };
}

AnalysisLodView AnalysisPyramid::view(
    media::MediaTime start,
    media::MediaTime end,
    const std::size_t maximumBuckets) const
{
    std::shared_lock lock(mutex_);
    AnalysisLodView result;
    if (levels_.empty()) {
        return result;
    }
    start = std::clamp(start, media::MediaTime::zero(), duration_);
    end = std::clamp(end, media::MediaTime::zero(), duration_);
    if (end < start) {
        std::swap(start, end);
    }
    result.rangeStart = start;
    result.rangeEnd = end;

    const std::size_t budget = std::max<std::size_t>(1, maximumBuckets);
    std::size_t selectedLevel = 0;
    for (; selectedLevel + 1 < levels_.size(); ++selectedLevel) {
        const std::size_t first = bucketIndex(start, selectedLevel);
        const std::size_t last = bucketIndex(end, selectedLevel);
        if (last - first + 1 <= budget) {
            break;
        }
    }

    result.level = selectedLevel;
    std::size_t sourceBuckets = 1;
    for (std::size_t level = 0; level < selectedLevel; ++level) {
        if (sourceBuckets > std::numeric_limits<std::size_t>::max() / config_.groupingFactor) {
            sourceBuckets = std::numeric_limits<std::size_t>::max();
            break;
        }
        sourceBuckets *= config_.groupingFactor;
    }
    result.sourceBucketsPerBucket = sourceBuckets;
    const std::int64_t span = levelSpan(selectedLevel);
    result.bucketDuration = media::MediaTime(span);
    const std::size_t first = bucketIndex(start, selectedLevel);
    const std::size_t last = bucketIndex(end, selectedLevel);
    result.buckets.reserve(std::min(budget, last - first + 1));

    for (std::size_t index = first; index <= last; ++index) {
        const auto& source = levels_[selectedLevel][index];
        if (source.sampleCount == 0) {
            continue;
        }
        AnalysisBucket bucket;
        bucket.start = bucketBoundary(index, span);
        bucket.end = bucketBoundary(index + 1, span);
        bucket.sampleCount = source.sampleCount;
        bucket.motionCount = source.motionCount;
        bucket.similarityCount = source.similarityCount;
        if (source.motionCount > 0) {
            bucket.minMotion = source.minMotion;
            bucket.maxMotion = source.maxMotion;
            bucket.averageMotion = static_cast<float>(
                source.motionSum / static_cast<double>(source.motionCount));
        }
        if (source.similarityCount > 0) {
            bucket.minSimilarity = source.minSimilarity;
            bucket.maxSimilarity = source.maxSimilarity;
            bucket.averageSimilarity = static_cast<float>(
                source.similaritySum / static_cast<double>(source.similarityCount));
        }
        result.buckets.push_back(bucket);
    }
    return result;
}

std::size_t AnalysisPyramid::baseBucketCount() const noexcept
{
    std::shared_lock lock(mutex_);
    return levels_.empty() ? 0 : levels_.front().size();
}

std::size_t AnalysisPyramid::levelCount() const noexcept
{
    std::shared_lock lock(mutex_);
    return levels_.size();
}

} // namespace vidscope::analysis
