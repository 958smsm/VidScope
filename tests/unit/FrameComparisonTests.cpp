#include "TestHarness.h"

#include "core/Cancellation.h"
#include "inspection/FrameComparison.h"

#include <QtGui/QColor>
#include <QtGui/QImage>

#include <cmath>

namespace {

using vidscope::inspection::ComparisonMode;
using vidscope::inspection::FrameComparison;

[[nodiscard]] QImage solidImage(const QSize size, const QColor color)
{
    QImage image(size, QImage::Format_RGBA8888);
    image.fill(color);
    return image;
}

} // namespace

VIDSCOPE_TEST(FrameComparison_identical_frames_have_perfect_metrics_and_black_difference)
{
    const QImage frame = solidImage(QSize(24, 16), QColor(42, 110, 203));
    const auto result = FrameComparison::analyze(
        frame,
        frame,
        ComparisonMode::AbsoluteDifference);
    VIDSCOPE_REQUIRE(result.metrics.comparable);
    VIDSCOPE_REQUIRE(result.metrics.ssim == 1.0);
    VIDSCOPE_REQUIRE(std::isinf(result.metrics.psnrDb));
    VIDSCOPE_REQUIRE(result.metrics.meanSquaredError == 0.0);
    VIDSCOPE_REQUIRE(result.visualization.size() == frame.size());
    VIDSCOPE_REQUIRE(result.visualization.pixelColor(5, 7) == QColor(0, 0, 0));
}

VIDSCOPE_TEST(FrameComparison_black_and_white_frames_have_expected_psnr_and_difference)
{
    const QImage black = solidImage(QSize(16, 16), Qt::black);
    const QImage white = solidImage(QSize(16, 16), Qt::white);
    const auto result = FrameComparison::analyze(
        black,
        white,
        ComparisonMode::AmplifiedDifference);
    VIDSCOPE_REQUIRE(result.metrics.comparable);
    VIDSCOPE_REQUIRE(result.metrics.ssim < 0.001);
    VIDSCOPE_REQUIRE(std::abs(result.metrics.psnrDb) < 0.0001);
    VIDSCOPE_REQUIRE(std::abs(result.metrics.meanSquaredError - 65'025.0) < 0.0001);
    VIDSCOPE_REQUIRE(result.visualization.pixelColor(8, 8) == QColor(255, 255, 255));
}

VIDSCOPE_TEST(FrameComparison_ssim_map_is_bounded_and_matches_source_dimensions)
{
    QImage first = solidImage(QSize(17, 13), QColor(80, 90, 100));
    QImage second = first;
    second.setPixelColor(0, 0, QColor(200, 10, 30));
    const auto result = FrameComparison::analyze(
        first,
        second,
        ComparisonMode::SsimMap);
    VIDSCOPE_REQUIRE(result.metrics.comparable);
    VIDSCOPE_REQUIRE(result.metrics.ssim >= 0.0);
    VIDSCOPE_REQUIRE(result.metrics.ssim < 1.0);
    VIDSCOPE_REQUIRE(result.visualization.size() == first.size());
    VIDSCOPE_REQUIRE(!result.visualization.isNull());
}

VIDSCOPE_TEST(FrameComparison_rejects_mixed_dimensions_without_resampling)
{
    const auto result = FrameComparison::analyze(
        solidImage(QSize(16, 16), Qt::black),
        solidImage(QSize(12, 16), Qt::black),
        ComparisonMode::AbsoluteDifference);
    VIDSCOPE_REQUIRE(!result.metrics.comparable);
    VIDSCOPE_REQUIRE(!result.metrics.detail.isEmpty());
    VIDSCOPE_REQUIRE(result.visualization.isNull());
}

VIDSCOPE_TEST(FrameComparison_honors_pre_requested_cancellation)
{
    vidscope::core::CancellationSource source;
    source.requestCancellation();
    const auto result = FrameComparison::analyze(
        solidImage(QSize(16, 16), Qt::black),
        solidImage(QSize(16, 16), Qt::white),
        ComparisonMode::SsimMap,
        4,
        source.token());
    VIDSCOPE_REQUIRE(result.cancelled);
    VIDSCOPE_REQUIRE(result.visualization.isNull());
}

