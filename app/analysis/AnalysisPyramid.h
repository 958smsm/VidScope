#pragma once

#include "analysis/AnalysisTypes.h"

#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <span>
#include <vector>

namespace vidscope::analysis {

struct AnalysisBucket final {
    media::MediaTime start{};
    media::MediaTime end{};
    std::uint64_t sampleCount = 0;
    std::uint64_t motionCount = 0;
    std::uint64_t similarityCount = 0;
    float minMotion = 0.0F;
    float maxMotion = 0.0F;
    float averageMotion = 0.0F;
    float minSimilarity = 0.0F;
    float maxSimilarity = 0.0F;
    float averageSimilarity = 0.0F;

    friend bool operator==(const AnalysisBucket&, const AnalysisBucket&) = default;
};

struct AnalysisLodView final {
    media::MediaTime rangeStart{};
    media::MediaTime rangeEnd{};
    media::MediaTime bucketDuration{};
    std::size_t level = 0;
    std::size_t sourceBucketsPerBucket = 1;
    std::vector<AnalysisBucket> buckets;
};

struct AnalysisPyramidConfig final {
    std::size_t maximumBaseBuckets = 262'144;
    std::size_t groupingFactor = 4;
    std::size_t maximumLevels = 16;
};

struct AnalysisPyramidRange final {
    media::MediaTime start{};
    media::MediaTime end{};
};

// Thread-safe temporal LOD hierarchy. Level zero is capped independently of
// the raw sample store; each higher level aggregates a configurable number of
// child buckets. Sparse analysis coverage remains sparse in returned views.
class AnalysisPyramid final {
public:
    explicit AnalysisPyramid(AnalysisPyramidConfig config = {});

    void reset(media::MediaTime duration, std::size_t estimatedSamples);
    void clear();
    void rebuild(std::span<const AnalysisSample> samples);
    void replaceRange(
        media::MediaTime start,
        media::MediaTime end,
        std::span<const AnalysisSample> samples);

    [[nodiscard]] AnalysisPyramidRange alignedBaseRange(
        media::MediaTime start,
        media::MediaTime end) const noexcept;
    [[nodiscard]] AnalysisLodView view(
        media::MediaTime start,
        media::MediaTime end,
        std::size_t maximumBuckets) const;
    [[nodiscard]] std::size_t baseBucketCount() const noexcept;
    [[nodiscard]] std::size_t levelCount() const noexcept;

private:
    struct Accumulator final {
        std::uint64_t sampleCount = 0;
        std::uint64_t motionCount = 0;
        std::uint64_t similarityCount = 0;
        double motionSum = 0.0;
        double similaritySum = 0.0;
        float minMotion = 1.0F;
        float maxMotion = 0.0F;
        float minSimilarity = 1.0F;
        float maxSimilarity = 0.0F;
    };

    [[nodiscard]] std::int64_t levelSpan(std::size_t level) const noexcept;
    [[nodiscard]] std::size_t bucketIndex(
        media::MediaTime time,
        std::size_t level) const noexcept;
    [[nodiscard]] media::MediaTime bucketBoundary(
        std::size_t index,
        std::int64_t span) const noexcept;
    static void accumulate(Accumulator& target, const AnalysisSample& sample) noexcept;
    static void merge(Accumulator& target, const Accumulator& source) noexcept;
    void rebuildParents(std::size_t firstBaseBucket, std::size_t lastBaseBucket);

    AnalysisPyramidConfig config_;
    media::MediaTime duration_{};
    std::int64_t baseSpanNanoseconds_ = 0;
    std::vector<std::vector<Accumulator>> levels_;
    mutable std::shared_mutex mutex_;
};

} // namespace vidscope::analysis
