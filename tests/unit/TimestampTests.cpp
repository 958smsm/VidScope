#include "TestHarness.h"

#include "media/FfmpegRaii.h"
#include "media/MediaTypes.h"

#include <chrono>
#include <cmath>
#include <cstdint>

extern "C" {
#include <libavutil/mastering_display_metadata.h>
}

using namespace std::chrono_literals;

VIDSCOPE_TEST(Timestamp_normalizes_nonzero_stream_origin_exactly)
{
    const auto time = vidscope::media::timestampToMediaTime(91'125, 90'000, AVRational{1, 90'000});
    VIDSCOPE_REQUIRE(time == 12'500'000ns);
    VIDSCOPE_REQUIRE(vidscope::media::mediaTimeToTimestamp(time, 90'000, AVRational{1, 90'000}) == 91'125);
}

VIDSCOPE_TEST(Timestamp_round_trip_handles_long_media_without_floating_point)
{
    constexpr std::int64_t origin = 9'007'199'000;
    constexpr std::int64_t timestamp = origin + 987'654'321;
    constexpr AVRational timeBase{1, 90'000};
    const auto time = vidscope::media::timestampToMediaTime(timestamp, origin, timeBase);
    const auto roundTrip = vidscope::media::mediaTimeToTimestamp(time, origin, timeBase);
    VIDSCOPE_REQUIRE(roundTrip == timestamp || roundTrip == timestamp - 1 || roundTrip == timestamp + 1);
}

VIDSCOPE_TEST(Timestamp_nominal_duration_is_only_metadata_fallback)
{
    const auto duration = vidscope::media::nominalFrameDuration(AVRational{24'000, 1'001});
    VIDSCOPE_REQUIRE(duration >= 41'708'332ns);
    VIDSCOPE_REQUIRE(duration <= 41'708'334ns);
    VIDSCOPE_REQUIRE(vidscope::media::nominalFrameDuration(AVRational{0, 1}) == 0ns);
}

VIDSCOPE_TEST(HDR10_side_data_is_exposed_as_stable_frame_metadata)
{
    auto frame = vidscope::media::makeFrame();
    auto* mastering = av_mastering_display_metadata_create_side_data(frame.get());
    VIDSCOPE_REQUIRE(mastering != nullptr);
    mastering->has_primaries = 1;
    mastering->display_primaries[0][0] = AVRational{34'000, 50'000};
    mastering->display_primaries[0][1] = AVRational{16'000, 50'000};
    mastering->display_primaries[1][0] = AVRational{13'250, 50'000};
    mastering->display_primaries[1][1] = AVRational{34'500, 50'000};
    mastering->display_primaries[2][0] = AVRational{7'500, 50'000};
    mastering->display_primaries[2][1] = AVRational{3'000, 50'000};
    mastering->white_point[0] = AVRational{15'635, 50'000};
    mastering->white_point[1] = AVRational{16'450, 50'000};
    mastering->has_luminance = 1;
    mastering->min_luminance = AVRational{1, 10'000};
    mastering->max_luminance = AVRational{1'000, 1};

    auto* light = av_content_light_metadata_create_side_data(frame.get());
    VIDSCOPE_REQUIRE(light != nullptr);
    light->MaxCLL = 1'000;
    light->MaxFALL = 400;

    const auto masteringResult =
        vidscope::media::extractMasteringDisplayMetadata(*frame);
    VIDSCOPE_REQUIRE(masteringResult.has_value());
    VIDSCOPE_REQUIRE(masteringResult->primaries.has_value());
    VIDSCOPE_REQUIRE(masteringResult->luminance.has_value());
    VIDSCOPE_REQUIRE(std::abs(masteringResult->primaries->red.x - 0.68) < 1e-12);
    VIDSCOPE_REQUIRE(std::abs(masteringResult->primaries->green.y - 0.69) < 1e-12);
    VIDSCOPE_REQUIRE(std::abs(masteringResult->primaries->blue.x - 0.15) < 1e-12);
    VIDSCOPE_REQUIRE(std::abs(masteringResult->primaries->whitePoint.x - 0.3127) < 1e-12);
    VIDSCOPE_REQUIRE(std::abs(masteringResult->luminance->minimumNits - 0.0001) < 1e-12);
    VIDSCOPE_REQUIRE(std::abs(masteringResult->luminance->maximumNits - 1'000.0) < 1e-12);

    const auto lightResult = vidscope::media::extractContentLightMetadata(*frame);
    VIDSCOPE_REQUIRE(lightResult.has_value());
    VIDSCOPE_REQUIRE(lightResult->maxContentLightLevel == 1'000);
    VIDSCOPE_REQUIRE(lightResult->maxFrameAverageLightLevel == 400);
}

VIDSCOPE_TEST(HDR10_extractors_report_absent_side_data_without_fabricating_values)
{
    const auto frame = vidscope::media::makeFrame();
    VIDSCOPE_REQUIRE(!vidscope::media::extractMasteringDisplayMetadata(*frame).has_value());
    VIDSCOPE_REQUIRE(!vidscope::media::extractContentLightMetadata(*frame).has_value());
}


