#pragma once

#include "analysis/AnalysisPyramid.h"

#include <QtCore/QRectF>

#include <cstdint>
#include <optional>

class QPainter;

namespace vidscope::timeline {

enum class HeatmapMode : std::uint8_t {
    Motion,
    Similarity,
    Combined,
};

struct CombinedHeatmapWeights final {
    float motion = 0.50F;
    float similarityDifference = 0.30F;

    friend bool operator==(
        const CombinedHeatmapWeights&,
        const CombinedHeatmapWeights&) = default;
};

// Stateless painter for bounded LOD views. Raw values and combination policy
// stay outside the renderer; no decoding, aggregation, or cache access occurs
// here.
class TimelineHeatmapRenderer final {
public:
    void paint(
        QPainter& painter,
        const QRectF& bounds,
        const analysis::AnalysisLodView& view,
        HeatmapMode mode,
        CombinedHeatmapWeights weights = {}) const;

    [[nodiscard]] static std::optional<float> averageScore(
        const analysis::AnalysisBucket& bucket,
        HeatmapMode mode,
        CombinedHeatmapWeights weights = {}) noexcept;
    [[nodiscard]] static std::optional<float> peakScore(
        const analysis::AnalysisBucket& bucket,
        HeatmapMode mode,
        CombinedHeatmapWeights weights = {}) noexcept;
};

} // namespace vidscope::timeline
