#pragma once

#include "media/MediaTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <vector>

namespace vidscope::analysis {

enum class AnalysisPriority : std::uint8_t {
    AroundPlayhead = 0,
    VisibleRange = 1,
    Background = 2,
};

enum class AnalysisState : std::uint8_t {
    Idle,
    LoadingCache,
    Analyzing,
    Paused,
    Complete,
    Error,
};

struct AnalysisSample final {
    media::MediaTime presentationTime{};
    media::MediaTime duration{};
    std::int64_t presentationIndex = -1;
    std::int64_t pts = AV_NOPTS_VALUE;
    bool keyFrame = false;
    std::optional<float> motion;
    std::optional<float> similarity;

    friend bool operator==(const AnalysisSample&, const AnalysisSample&) = default;
};

// A compact, thread-safe raw-sample store. Phase 7 will build LOD aggregates
// above this layer; Phase 6 deliberately keeps the original scores intact.
class AnalysisStore final {
public:
    explicit AnalysisStore(std::size_t maximumSamples = 2'000'000);

    [[nodiscard]] bool upsert(AnalysisSample sample);
    [[nodiscard]] std::size_t merge(std::vector<AnalysisSample> samples);
    void replace(std::vector<AnalysisSample> samples);
    void clear();

    [[nodiscard]] std::optional<AnalysisSample> nearest(
        media::MediaTime time,
        std::int64_t presentationIndex = -1,
        media::MediaTime maximumDistance = std::chrono::milliseconds(500)) const;
    [[nodiscard]] std::vector<AnalysisSample> range(
        media::MediaTime start,
        media::MediaTime end,
        std::size_t maximumResults = 100'000) const;
    [[nodiscard]] std::vector<AnalysisSample> snapshot() const;
    [[nodiscard]] std::optional<media::MediaTime> latestPresentationEnd() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept;

private:
    [[nodiscard]] static bool orderedBefore(
        const AnalysisSample& left,
        const AnalysisSample& right) noexcept;
    [[nodiscard]] static bool sameIdentity(
        const AnalysisSample& left,
        const AnalysisSample& right) noexcept;

    const std::size_t maximumSamples_;
    mutable std::shared_mutex mutex_;
    std::vector<AnalysisSample> samples_;
};

} // namespace vidscope::analysis
