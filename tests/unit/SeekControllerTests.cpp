#include "TestHarness.h"

#include "playback/SeekController.h"

#include <chrono>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

vidscope::media::DecodedFrame frameAt(std::chrono::nanoseconds time)
{
    vidscope::media::DecodedFrame frame;
    frame.presentationTime = time;
    return frame;
}

} // namespace

VIDSCOPE_TEST(SeekController_plans_in_stream_time_base_with_nonzero_origin)
{
    vidscope::media::MediaInfo info;
    info.timeBase = AVRational{1, 90'000};
    info.streamStartTimestamp = 90'000;
    info.duration = 10s;

    vidscope::playback::SeekRequest request{7, 2s, vidscope::playback::SeekBias::AtOrAfter};
    const auto plan = vidscope::playback::SeekController::plan(info, request);
    VIDSCOPE_REQUIRE(plan.targetStreamTimestamp == 270'000);
    VIDSCOPE_REQUIRE(plan.maximumStreamTimestamp == 270'000);
    VIDSCOPE_REQUIRE(!plan.startsAtStreamOrigin);
}

VIDSCOPE_TEST(SeekController_clamps_targets_to_media_extent)
{
    vidscope::media::MediaInfo info;
    info.timeBase = AVRational{1, 1'000};
    info.streamStartTimestamp = 5'000;
    info.duration = 3s;

    auto negative = vidscope::playback::SeekController::plan(
        info, {1, -4s, vidscope::playback::SeekBias::AtOrAfter});
    VIDSCOPE_REQUIRE(negative.targetStreamTimestamp == 5'000);
    VIDSCOPE_REQUIRE(negative.startsAtStreamOrigin);

    auto beyond = vidscope::playback::SeekController::plan(
        info, {2, 30s, vidscope::playback::SeekBias::AtOrAfter});
    VIDSCOPE_REQUIRE(beyond.targetStreamTimestamp == 8'000);
}

VIDSCOPE_TEST(SeekController_floors_bracketing_biases_in_a_coarse_time_base)
{
    vidscope::media::MediaInfo info;
    info.timeBase = AVRational{1, 10};
    info.streamStartTimestamp = 100;
    info.duration = 2s;

    const auto after = vidscope::playback::SeekController::plan(
        info, {1, 60ms, vidscope::playback::SeekBias::AtOrAfter});
    const auto before = vidscope::playback::SeekController::plan(
        info, {2, 60ms, vidscope::playback::SeekBias::AtOrBefore});
    const auto nearest = vidscope::playback::SeekController::plan(
        info, {3, 60ms, vidscope::playback::SeekBias::Nearest});
    const auto exact = vidscope::playback::SeekController::plan(
        info, {4, 100ms, vidscope::playback::SeekBias::AtOrBefore});

    VIDSCOPE_REQUIRE(after.targetStreamTimestamp == 101);
    VIDSCOPE_REQUIRE(before.targetStreamTimestamp == 100);
    VIDSCOPE_REQUIRE(nearest.targetStreamTimestamp == 100);
    VIDSCOPE_REQUIRE(exact.targetStreamTimestamp == 101);
}

VIDSCOPE_TEST(SeekController_applies_requested_selection_bias)
{
    const auto frame = frameAt(150ms);
    VIDSCOPE_REQUIRE(vidscope::playback::SeekController::frameSatisfies(
        frame, {1, 100ms, vidscope::playback::SeekBias::AtOrAfter}));
    VIDSCOPE_REQUIRE(!vidscope::playback::SeekController::frameSatisfies(
        frame, {1, 100ms, vidscope::playback::SeekBias::AtOrBefore}));
    VIDSCOPE_REQUIRE(vidscope::playback::SeekController::frameSatisfies(
        frame, {1, 150ms, vidscope::playback::SeekBias::Nearest}));
}

VIDSCOPE_TEST(RequestGate_accepts_only_the_newest_generation)
{
    vidscope::playback::RequestGate gate;
    const auto first = gate.next();
    const auto second = gate.next();
    VIDSCOPE_REQUIRE(second > first);
    VIDSCOPE_REQUIRE(!gate.accepts(first));
    VIDSCOPE_REQUIRE(gate.accepts(second));
    VIDSCOPE_REQUIRE(gate.current() == second);
}


