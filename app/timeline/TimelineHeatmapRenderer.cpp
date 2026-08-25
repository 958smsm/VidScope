#include "timeline/TimelineHeatmapRenderer.h"

#include <QtGui/QColor>
#include <QtGui/QPainter>
#include <QtGui/QPen>

#include <algorithm>
#include <cmath>

namespace vidscope::timeline {
namespace {

[[nodiscard]] std::optional<float> combinedScore(
    const std::optional<float> motion,
    const std::optional<float> similarityDifference,
    const std::optional<float> sceneChange,
    const CombinedHeatmapWeights weights) noexcept
{
    float weightedScore = 0.0F;
    float activeWeight = 0.0F;
    const float motionWeight = std::max(0.0F, weights.motion);
    const float similarityWeight = std::max(0.0F, weights.similarityDifference);
    const float sceneWeight = std::max(0.0F, weights.sceneChange);
    if (motion && motionWeight > 0.0F) {
        weightedScore += motionWeight * std::clamp(*motion, 0.0F, 1.0F);
        activeWeight += motionWeight;
    }
    if (similarityDifference && similarityWeight > 0.0F) {
        weightedScore += similarityWeight * std::clamp(*similarityDifference, 0.0F, 1.0F);
        activeWeight += similarityWeight;
    }
    if (sceneChange && sceneWeight > 0.0F) {
        weightedScore += sceneWeight * std::clamp(*sceneChange, 0.0F, 1.0F);
        activeWeight += sceneWeight;
    }
    if (activeWeight <= 0.0F) {
        return std::nullopt;
    }
    return std::clamp(weightedScore / activeWeight, 0.0F, 1.0F);
}

[[nodiscard]] QColor lowColor(const HeatmapMode mode) noexcept
{
    switch (mode) {
    case HeatmapMode::Motion:
        return QColor(35, 63, 86);
    case HeatmapMode::Similarity:
        return QColor(45, 55, 82);
    case HeatmapMode::SceneChange:
        return QColor(67, 45, 76);
    case HeatmapMode::Combined:
        return QColor(50, 49, 77);
    }
    return QColor(40, 48, 61);
}

[[nodiscard]] QColor highColor(const HeatmapMode mode) noexcept
{
    switch (mode) {
    case HeatmapMode::Motion:
        return QColor(255, 133, 69);
    case HeatmapMode::Similarity:
        return QColor(83, 220, 170);
    case HeatmapMode::SceneChange:
        return QColor(255, 92, 180);
    case HeatmapMode::Combined:
        return QColor(224, 105, 255);
    }
    return QColor(220, 225, 235);
}

[[nodiscard]] QColor scoreColor(const HeatmapMode mode, const float score) noexcept
{
    const float amount = std::clamp(score, 0.0F, 1.0F);
    const QColor low = lowColor(mode);
    const QColor high = highColor(mode);
    return QColor(
        static_cast<int>(std::lround(low.red() + (high.red() - low.red()) * amount)),
        static_cast<int>(std::lround(low.green() + (high.green() - low.green()) * amount)),
        static_cast<int>(std::lround(low.blue() + (high.blue() - low.blue()) * amount)),
        static_cast<int>(std::lround(90.0F + 150.0F * amount)));
}

} // namespace

std::optional<float> TimelineHeatmapRenderer::averageScore(
    const analysis::AnalysisBucket& bucket,
    const HeatmapMode mode,
    const CombinedHeatmapWeights weights) noexcept
{
    switch (mode) {
    case HeatmapMode::Motion:
        return bucket.motionCount > 0
            ? std::optional<float>(bucket.averageMotion)
            : std::nullopt;
    case HeatmapMode::Similarity:
        return bucket.similarityCount > 0
            ? std::optional<float>(bucket.averageSimilarity)
            : std::nullopt;
    case HeatmapMode::SceneChange:
        return bucket.sceneCount > 0
            ? std::optional<float>(bucket.averageSceneScore)
            : std::nullopt;
    case HeatmapMode::Combined:
        return combinedScore(
            bucket.motionCount > 0
                ? std::optional<float>(bucket.averageMotion)
                : std::nullopt,
            bucket.similarityCount > 0
                ? std::optional<float>(1.0F - bucket.averageSimilarity)
                : std::nullopt,
            bucket.sceneCount > 0
                ? std::optional<float>(bucket.averageSceneScore)
                : std::nullopt,
            weights);
    }
    return std::nullopt;
}

std::optional<float> TimelineHeatmapRenderer::peakScore(
    const analysis::AnalysisBucket& bucket,
    const HeatmapMode mode,
    const CombinedHeatmapWeights weights) noexcept
{
    switch (mode) {
    case HeatmapMode::Motion:
        return bucket.motionCount > 0
            ? std::optional<float>(bucket.maxMotion)
            : std::nullopt;
    case HeatmapMode::Similarity:
        return bucket.similarityCount > 0
            ? std::optional<float>(bucket.maxSimilarity)
            : std::nullopt;
    case HeatmapMode::SceneChange:
        return bucket.sceneCount > 0
            ? std::optional<float>(bucket.maxSceneScore)
            : std::nullopt;
    case HeatmapMode::Combined:
        return combinedScore(
            bucket.motionCount > 0
                ? std::optional<float>(bucket.maxMotion)
                : std::nullopt,
            bucket.similarityCount > 0
                ? std::optional<float>(1.0F - bucket.minSimilarity)
                : std::nullopt,
            bucket.sceneCount > 0
                ? std::optional<float>(bucket.maxSceneScore)
                : std::nullopt,
            weights);
    }
    return std::nullopt;
}

void TimelineHeatmapRenderer::paint(
    QPainter& painter,
    const QRectF& bounds,
    const analysis::AnalysisLodView& view,
    const HeatmapMode mode,
    const CombinedHeatmapWeights weights) const
{
    const auto rangeNanoseconds = view.rangeEnd.count() - view.rangeStart.count();
    if (bounds.isEmpty() || rangeNanoseconds <= 0 || view.buckets.empty()) {
        return;
    }

    const auto xForTime = [&](const media::MediaTime time) {
        const auto relative = time.count() - view.rangeStart.count();
        const long double ratio = std::clamp(
            static_cast<long double>(relative) / static_cast<long double>(rangeNanoseconds),
            0.0L,
            1.0L);
        return bounds.left() + bounds.width() * static_cast<qreal>(ratio);
    };

    painter.save();
    painter.setClipRect(bounds);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);

    for (const auto& bucket : view.buckets) {
        const auto average = averageScore(bucket, mode, weights);
        if (!average) {
            continue;
        }
        qreal left = xForTime(std::max(bucket.start, view.rangeStart));
        qreal right = xForTime(std::min(bucket.end, view.rangeEnd));
        if (right < left) {
            std::swap(left, right);
        }
        const qreal width = std::max<qreal>(1.0, right - left);
        const float value = std::clamp(*average, 0.0F, 1.0F);
        QColor color = scoreColor(mode, value);
        QColor wash = color;
        wash.setAlpha(std::max(24, color.alpha() / 4));
        painter.fillRect(QRectF(left, bounds.top(), width, bounds.height()), wash);

        const qreal barHeight = std::max<qreal>(1.0, bounds.height() * value);
        painter.fillRect(
            QRectF(left, bounds.bottom() - barHeight + 1.0, width, barHeight),
            color);

        if (const auto peak = peakScore(bucket, mode, weights)) {
            const qreal peakY = bounds.bottom()
                - bounds.height() * std::clamp(*peak, 0.0F, 1.0F);
            QColor peakColor = highColor(mode);
            peakColor.setAlpha(205);
            painter.setPen(QPen(peakColor, 1.0));
            painter.drawLine(QLineF(left, peakY, left + width, peakY));
            painter.setPen(Qt::NoPen);
        }
    }

    painter.restore();
}

} // namespace vidscope::timeline
