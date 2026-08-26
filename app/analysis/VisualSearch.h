#pragma once

#include "analysis/AnalysisTypes.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vidscope::analysis {

struct VisualMatch final {
    media::MediaTime presentationTime{};
    std::int64_t presentationIndex = -1;
    std::uint8_t hammingDistance = 0;
    float similarity = 0.0F;

    friend bool operator==(const VisualMatch&, const VisualMatch&) = default;
};

class VisualSearch final {
public:
    [[nodiscard]] static std::vector<VisualMatch> findSimilar(
        std::span<const AnalysisSample> samples,
        std::uint64_t queryHash,
        media::MediaTime queryTime,
        std::int64_t queryPresentationIndex = -1,
        std::size_t maximumResults = 48,
        std::uint8_t maximumHammingDistance = 24);
};

} // namespace vidscope::analysis
