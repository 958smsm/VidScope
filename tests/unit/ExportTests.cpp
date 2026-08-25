#include "TestHarness.h"

#include "export/ExportTypes.h"

#include <chrono>

using namespace std::chrono_literals;

namespace {

using vidscope::exporting::ContactSheetOptions;
using vidscope::exporting::ExportPlanner;
using vidscope::exporting::ExportRequest;
using vidscope::exporting::ImageFormat;
using vidscope::media::MediaTime;

} // namespace

VIDSCOPE_TEST(ExportPlanner_normalizes_ranges_limits_and_target_times)
{
    ExportRequest request;
    request.rangeStart = 8s;
    request.rangeEnd = 2s;
    request.everyNFrames = 0;
    request.maximumFrames = 0;
    request.quality = 140;
    request.baseName = QStringLiteral("  bad:name.  ");
    request.targetTimes = {-1s, 3s, 3s, 20s};
    request.contactSheet.rows = 80;
    request.contactSheet.columns = 80;
    request.contactSheet.frameCount = 5'000;
    request.contactSheet.cellSize = QSize(20, 4'000);

    const auto normalized = ExportPlanner::normalized(std::move(request), 10s);
    VIDSCOPE_REQUIRE(normalized.rangeStart == 2s);
    VIDSCOPE_REQUIRE(normalized.rangeEnd == 8s);
    VIDSCOPE_REQUIRE(normalized.everyNFrames == 1);
    VIDSCOPE_REQUIRE(normalized.maximumFrames == 1);
    VIDSCOPE_REQUIRE(normalized.quality == 100);
    VIDSCOPE_REQUIRE(normalized.baseName == QStringLiteral("bad_name"));
    VIDSCOPE_REQUIRE(normalized.targetTimes.size() == 1);
    VIDSCOPE_REQUIRE(normalized.targetTimes.front() == MediaTime::zero());
    VIDSCOPE_REQUIRE(normalized.contactSheet.rows == 32);
    VIDSCOPE_REQUIRE(normalized.contactSheet.columns == 32);
    VIDSCOPE_REQUIRE(normalized.contactSheet.frameCount == 1'024);
    VIDSCOPE_REQUIRE(normalized.contactSheet.cellSize == QSize(64, 1'080));
}

VIDSCOPE_TEST(ExportPlanner_evenly_spaced_times_use_exact_integer_endpoints)
{
    const auto times = ExportPlanner::evenlySpacedTimes(
        MediaTime(0),
        MediaTime(10),
        4);
    VIDSCOPE_REQUIRE(times.size() == 4);
    VIDSCOPE_REQUIRE(times[0] == MediaTime(0));
    VIDSCOPE_REQUIRE(times[1] == MediaTime(3));
    VIDSCOPE_REQUIRE(times[2] == MediaTime(6));
    VIDSCOPE_REQUIRE(times[3] == MediaTime(10));
    VIDSCOPE_REQUIRE(
        ExportPlanner::evenlySpacedTimes(2s, 8s, 1)
        == std::vector<MediaTime>{2s});
}

VIDSCOPE_TEST(ExportPlanner_contact_sheet_presets_and_pixel_bounds_are_deterministic)
{
    VIDSCOPE_REQUIRE(ExportPlanner::presetGrid(8) == QSize(4, 2));
    VIDSCOPE_REQUIRE(ExportPlanner::presetGrid(16) == QSize(4, 4));
    VIDSCOPE_REQUIRE(ExportPlanner::presetGrid(20) == QSize(5, 4));
    VIDSCOPE_REQUIRE(ExportPlanner::presetGrid(25) == QSize(5, 5));

    ContactSheetOptions options;
    VIDSCOPE_REQUIRE(
        ExportPlanner::contactSheetPixelSize(options)
        == QSize(1'664, 912));
    options.includeTimestamp = false;
    options.includeFrameIndex = false;
    VIDSCOPE_REQUIRE(
        ExportPlanner::contactSheetPixelSize(options)
        == QSize(1'664, 776));
    options.rows = 32;
    options.columns = 32;
    options.cellSize = QSize(1'920, 1'080);
    VIDSCOPE_REQUIRE(!ExportPlanner::contactSheetPixelSize(options).isValid());
}

VIDSCOPE_TEST(ExportPlanner_formats_and_names_cover_all_phase10_image_types)
{
    VIDSCOPE_REQUIRE(ExportPlanner::formatName(ImageFormat::Png) == QByteArrayLiteral("png"));
    VIDSCOPE_REQUIRE(ExportPlanner::extension(ImageFormat::Jpeg) == QStringLiteral("jpg"));
    VIDSCOPE_REQUIRE(ExportPlanner::extension(ImageFormat::WebP) == QStringLiteral("webp"));
    VIDSCOPE_REQUIRE(ExportPlanner::extension(ImageFormat::Bmp) == QStringLiteral("bmp"));
    VIDSCOPE_REQUIRE(ExportPlanner::extension(ImageFormat::Tiff) == QStringLiteral("tiff"));
    VIDSCOPE_REQUIRE(
        ExportPlanner::formatFromPath(std::filesystem::path("image.JPEG"))
        == ImageFormat::Jpeg);
    VIDSCOPE_REQUIRE(
        ExportPlanner::fileDialogFilter().contains(QStringLiteral("TIFF")));
}
