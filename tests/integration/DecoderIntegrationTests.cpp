#include "TestHarness.h"

#include "media/MediaTypes.h"
#include "playback/PlaybackSession.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/imgutils.h>
}

using namespace std::chrono_literals;

namespace {

std::filesystem::path fixtureDirectory;

vidscope::playback::PlaybackSessionConfig testConfig()
{
    vidscope::playback::PlaybackSessionConfig config;
    config.frameCacheBytes = 64U * 1024U * 1024U;
    config.forwardQueueBytes = 32U * 1024U * 1024U;
    config.forwardQueueFrames = 12;
    config.initialPrefetchFrames = 6;
    config.decoder.hardwareAcceleration = vidscope::media::HardwareAcceleration::Disabled;
    return config;
}

std::vector<std::uint8_t> packedFrameBytes(
    const vidscope::media::DecodedFrame& frame)
{
    VIDSCOPE_REQUIRE(frame.storage != nullptr);
    const AVFrame* surface = frame.storage->get();
    VIDSCOPE_REQUIRE(surface != nullptr);
    const auto format = static_cast<AVPixelFormat>(surface->format);
    const int byteCount =
        av_image_get_buffer_size(format, surface->width, surface->height, 1);
    VIDSCOPE_REQUIRE(byteCount > 0);

    const std::uint8_t* planes[4] = {
        surface->data[0],
        surface->data[1],
        surface->data[2],
        surface->data[3],
    };
    const int lineSizes[4] = {
        surface->linesize[0],
        surface->linesize[1],
        surface->linesize[2],
        surface->linesize[3],
    };
    std::vector<std::uint8_t> packed(static_cast<std::size_t>(byteCount));
    const int copied = av_image_copy_to_buffer(
        packed.data(),
        byteCount,
        planes,
        lineSizes,
        format,
        surface->width,
        surface->height,
        1);
    VIDSCOPE_REQUIRE(copied == byteCount);
    return packed;
}

void requireSameFrameContent(
    const vidscope::media::DecodedFrame& actual,
    const vidscope::media::DecodedFrame& expected)
{
    VIDSCOPE_REQUIRE(packedFrameBytes(actual) == packedFrameBytes(expected));
}

std::vector<vidscope::media::DecodedFramePtr> decodeAll(const std::filesystem::path& path)
{
    vidscope::playback::PlaybackSession session(testConfig());
    const auto opened = session.open(path);
    VIDSCOPE_REQUIRE_MESSAGE(static_cast<bool>(opened), path.string());

    std::vector<vidscope::media::DecodedFramePtr> frames{opened.frame};
    for (std::size_t guard = 0; guard < 10'000; ++guard) {
        const auto next = session.nextFrame();
        if (next.status == vidscope::playback::NavigationStatus::EndOfStream) {
            return frames;
        }
        VIDSCOPE_REQUIRE(next.status == vidscope::playback::NavigationStatus::FrameReady);
        VIDSCOPE_REQUIRE(next.frame != nullptr);
        VIDSCOPE_REQUIRE(next.frame->presentationTime > frames.back()->presentationTime);
        frames.push_back(next.frame);
    }
    vidscope::test::fail("decoder reaches EOF", __FILE__, __LINE__, path.string());
}

void verifySeekAndReverse(const std::string& fixture, std::chrono::nanoseconds target)
{
    const auto path = fixtureDirectory / fixture;
    const auto reference = decodeAll(path);
    const auto expected = std::lower_bound(
        reference.begin(), reference.end(), target, [](const auto& frame, const auto& time) {
            return frame->presentationTime < time;
        });
    VIDSCOPE_REQUIRE(expected != reference.end());
    VIDSCOPE_REQUIRE(expected != reference.begin());

    vidscope::playback::PlaybackSession session(testConfig());
    VIDSCOPE_REQUIRE(static_cast<bool>(session.open(path)));
    const auto seek = session.seek({42, target, vidscope::playback::SeekBias::AtOrAfter});
    VIDSCOPE_REQUIRE(static_cast<bool>(seek));
    VIDSCOPE_REQUIRE(seek.frame->presentationTime == (*expected)->presentationTime);
    requireSameFrameContent(*seek.frame, **expected);

    const auto previous = session.previousFrame();
    VIDSCOPE_REQUIRE(static_cast<bool>(previous));
    VIDSCOPE_REQUIRE(previous.frame->presentationTime == (*std::prev(expected))->presentationTime);
    requireSameFrameContent(*previous.frame, **std::prev(expected));

    const auto next = session.nextFrame();
    VIDSCOPE_REQUIRE(static_cast<bool>(next));
    VIDSCOPE_REQUIRE(next.frame->presentationTime == (*expected)->presentationTime);
    requireSameFrameContent(*next.frame, **expected);
}

} // namespace

VIDSCOPE_TEST(CFR_fixture_has_exact_presentation_count_and_monotonic_timestamps)
{
    const auto frames = decodeAll(fixtureDirectory / "cfr_no_b.mp4");
    VIDSCOPE_REQUIRE(frames.size() == 24);
    VIDSCOPE_REQUIRE(frames.front()->id.presentationIndex == 0);
    VIDSCOPE_REQUIRE(frames.back()->id.presentationIndex == 23);
}

VIDSCOPE_TEST(B_frame_fixture_is_emitted_in_presentation_order_and_drained_at_EOF)
{
    const auto frames = decodeAll(fixtureDirectory / "cfr_bframes.mp4");
    VIDSCOPE_REQUIRE(frames.size() == 24);
    VIDSCOPE_REQUIRE(std::any_of(frames.begin(), frames.end(), [](const auto& frame) {
        return frame->pictureType == AV_PICTURE_TYPE_B;
    }));
}

VIDSCOPE_TEST(VFR_fixture_preserves_more_than_one_actual_frame_duration)
{
    const auto frames = decodeAll(fixtureDirectory / "vfr.mp4");
    VIDSCOPE_REQUIRE(frames.size() >= 18);
    std::set<std::int64_t> deltas;
    std::set<std::int64_t> declaredDurations;
    for (std::size_t index = 1; index < frames.size(); ++index) {
        VIDSCOPE_REQUIRE(frames[index - 1]->duration > vidscope::media::MediaTime::zero());
        declaredDurations.insert(frames[index - 1]->duration.count());
        deltas.insert((frames[index]->presentationTime - frames[index - 1]->presentationTime).count());
    }
    VIDSCOPE_REQUIRE(frames.back()->duration > vidscope::media::MediaTime::zero());
    declaredDurations.insert(frames.back()->duration.count());
    VIDSCOPE_REQUIRE(deltas.size() >= 2);
    VIDSCOPE_REQUIRE(declaredDurations.size() >= 2);
}

VIDSCOPE_TEST(Coarse_time_base_seek_brackets_AtOrBefore_AtOrAfter_and_Nearest)
{
    const auto path = fixtureDirectory / "coarse_all_i.mp4";
    const auto reference = decodeAll(path);
    VIDSCOPE_REQUIRE(reference.size() >= 3);

    vidscope::playback::PlaybackSession session(testConfig());
    VIDSCOPE_REQUIRE(static_cast<bool>(session.open(path)));
    const auto* info = session.mediaInfo();
    VIDSCOPE_REQUIRE(info != nullptr);
    VIDSCOPE_REQUIRE(info->timeBase.num == 1);
    VIDSCOPE_REQUIRE(info->timeBase.den == 10);

    const auto before = session.seek(
        {10, 60ms, vidscope::playback::SeekBias::AtOrBefore});
    VIDSCOPE_REQUIRE(static_cast<bool>(before));
    requireSameFrameContent(*before.frame, *reference[0]);

    const auto nearest = session.seek(
        {11, 60ms, vidscope::playback::SeekBias::Nearest});
    VIDSCOPE_REQUIRE(static_cast<bool>(nearest));
    requireSameFrameContent(*nearest.frame, *reference[1]);

    const auto tied = session.seek(
        {12, 50ms, vidscope::playback::SeekBias::Nearest});
    VIDSCOPE_REQUIRE(static_cast<bool>(tied));
    requireSameFrameContent(*tied.frame, *reference[0]);

    const auto after = session.seek(
        {13, 60ms, vidscope::playback::SeekBias::AtOrAfter});
    VIDSCOPE_REQUIRE(static_cast<bool>(after));
    requireSameFrameContent(*after.frame, *reference[1]);
}

VIDSCOPE_TEST(Seek_near_beginning_uses_the_correct_surrounding_frame)
{
    const auto path = fixtureDirectory / "cfr_no_b.mp4";
    const auto reference = decodeAll(path);
    VIDSCOPE_REQUIRE(reference.size() >= 2);

    vidscope::playback::PlaybackSession session(testConfig());
    VIDSCOPE_REQUIRE(static_cast<bool>(session.open(path)));

    const auto before = session.seek(
        {20, 1ns, vidscope::playback::SeekBias::AtOrBefore});
    VIDSCOPE_REQUIRE(static_cast<bool>(before));
    requireSameFrameContent(*before.frame, *reference[0]);

    const auto nearest = session.seek(
        {21, 1ns, vidscope::playback::SeekBias::Nearest});
    VIDSCOPE_REQUIRE(static_cast<bool>(nearest));
    requireSameFrameContent(*nearest.frame, *reference[0]);

    const auto after = session.seek(
        {22, 1ns, vidscope::playback::SeekBias::AtOrAfter});
    VIDSCOPE_REQUIRE(static_cast<bool>(after));
    requireSameFrameContent(*after.frame, *reference[1]);
}

VIDSCOPE_TEST(Seek_at_duration_publishes_the_final_frame_and_preserves_reverse_step)
{
    const auto path = fixtureDirectory / "cfr_bframes.mp4";
    const auto reference = decodeAll(path);
    VIDSCOPE_REQUIRE(reference.size() >= 2);

    vidscope::playback::PlaybackSession session(testConfig());
    VIDSCOPE_REQUIRE(static_cast<bool>(session.open(path)));
    const auto* info = session.mediaInfo();
    VIDSCOPE_REQUIRE(info != nullptr);
    const auto duration = info->duration;
    VIDSCOPE_REQUIRE(duration > reference.back()->presentationTime);

    const auto final = session.seek(
        {30, duration, vidscope::playback::SeekBias::AtOrAfter});
    VIDSCOPE_REQUIRE(static_cast<bool>(final));
    requireSameFrameContent(*final.frame, *reference.back());

    const auto previous = session.previousFrame();
    VIDSCOPE_REQUIRE(static_cast<bool>(previous));
    requireSameFrameContent(*previous.frame, *reference[reference.size() - 2]);

    const auto next = session.nextFrame();
    VIDSCOPE_REQUIRE(static_cast<bool>(next));
    requireSameFrameContent(*next.frame, *reference.back());
    VIDSCOPE_REQUIRE(
        session.nextFrame().status
        == vidscope::playback::NavigationStatus::EndOfStream);
}

VIDSCOPE_TEST(VFR_boundary_seek_uses_actual_presentation_timestamps)
{
    const auto path = fixtureDirectory / "vfr.mp4";
    const auto reference = decodeAll(path);
    VIDSCOPE_REQUIRE(reference.size() >= 8);
    const auto target = reference[5]->presentationTime + 1ns;

    vidscope::playback::PlaybackSession session(testConfig());
    VIDSCOPE_REQUIRE(static_cast<bool>(session.open(path)));

    const auto before = session.seek(
        {40, target, vidscope::playback::SeekBias::AtOrBefore});
    VIDSCOPE_REQUIRE(static_cast<bool>(before));
    requireSameFrameContent(*before.frame, *reference[5]);

    const auto nearest = session.seek(
        {41, target, vidscope::playback::SeekBias::Nearest});
    VIDSCOPE_REQUIRE(static_cast<bool>(nearest));
    requireSameFrameContent(*nearest.frame, *reference[5]);

    const auto after = session.seek(
        {42, target, vidscope::playback::SeekBias::AtOrAfter});
    VIDSCOPE_REQUIRE(static_cast<bool>(after));
    requireSameFrameContent(*after.frame, *reference[6]);
}

VIDSCOPE_TEST(Tiny_cache_reconstructs_exact_reverse_and_forward_frames_after_eviction)
{
    const auto path = fixtureDirectory / "cfr_bframes.mp4";
    const auto reference = decodeAll(path);
    VIDSCOPE_REQUIRE(reference.size() > 18);
    const auto frameBytes = reference.front()->estimatedBytes();
    VIDSCOPE_REQUIRE(frameBytes > 0);

    auto config = testConfig();
    config.frameCacheBytes = frameBytes * 2;
    config.forwardQueueBytes = frameBytes * 3;
    config.forwardQueueFrames = 3;
    config.initialPrefetchFrames = 3;
    config.presentationIndexAnchorCount = 4;

    vidscope::playback::PlaybackSession session(config);
    const auto opened = session.open(path);
    VIDSCOPE_REQUIRE(static_cast<bool>(opened));
    requireSameFrameContent(*opened.frame, *reference[0]);

    for (std::size_t index = 1; index <= 18; ++index) {
        const auto next = session.nextFrame();
        VIDSCOPE_REQUIRE(static_cast<bool>(next));
        requireSameFrameContent(*next.frame, *reference[index]);
    }
    for (std::size_t offset = 1; offset <= 6; ++offset) {
        const auto previous = session.previousFrame();
        VIDSCOPE_REQUIRE(static_cast<bool>(previous));
        requireSameFrameContent(*previous.frame, *reference[18 - offset]);
    }
    const auto forward = session.nextFrame();
    VIDSCOPE_REQUIRE(static_cast<bool>(forward));
    requireSameFrameContent(*forward.frame, *reference[13]);
}

VIDSCOPE_TEST(Next_and_previous_keyframe_navigation_selects_actual_decoded_keyframes)
{
    const auto path = fixtureDirectory / "long_gop.mp4";
    const auto reference = decodeAll(path);
    VIDSCOPE_REQUIRE(reference.size() > 50);
    VIDSCOPE_REQUIRE(reference[0]->keyFrame);
    VIDSCOPE_REQUIRE(reference[50]->keyFrame);

    vidscope::playback::PlaybackSession session(testConfig());
    VIDSCOPE_REQUIRE(static_cast<bool>(session.open(path)));
    for (std::size_t index = 1; index <= 12; ++index) {
        VIDSCOPE_REQUIRE(static_cast<bool>(session.nextFrame()));
    }

    const auto nextKeyframe = session.nextKeyframe();
    VIDSCOPE_REQUIRE(static_cast<bool>(nextKeyframe));
    requireSameFrameContent(*nextKeyframe.frame, *reference[50]);

    const auto previousKeyframe = session.previousKeyframe();
    VIDSCOPE_REQUIRE(static_cast<bool>(previousKeyframe));
    requireSameFrameContent(*previousKeyframe.frame, *reference[0]);
}

VIDSCOPE_TEST(Tiny_cache_previous_keyframe_then_next_does_not_jump_over_evicted_frames)
{
    const auto path = fixtureDirectory / "long_gop.mp4";
    const auto reference = decodeAll(path);
    VIDSCOPE_REQUIRE(reference.size() > 58);
    const auto frameBytes = reference.front()->estimatedBytes();
    VIDSCOPE_REQUIRE(frameBytes > 0);

    auto config = testConfig();
    config.frameCacheBytes = frameBytes * 2;
    config.forwardQueueBytes = frameBytes * 2;
    config.forwardQueueFrames = 2;
    config.initialPrefetchFrames = 2;
    config.presentationIndexAnchorCount = 4;

    vidscope::playback::PlaybackSession session(config);
    VIDSCOPE_REQUIRE(static_cast<bool>(session.open(path)));
    for (std::size_t index = 1; index <= 58; ++index) {
        const auto next = session.nextFrame();
        VIDSCOPE_REQUIRE(static_cast<bool>(next));
        requireSameFrameContent(*next.frame, *reference[index]);
    }

    const auto keyframe = session.previousKeyframe();
    VIDSCOPE_REQUIRE(static_cast<bool>(keyframe));
    VIDSCOPE_REQUIRE(keyframe.frame->keyFrame);
    requireSameFrameContent(*keyframe.frame, *reference[50]);

    const auto next = session.nextFrame();
    VIDSCOPE_REQUIRE(static_cast<bool>(next));
    requireSameFrameContent(*next.frame, *reference[51]);
}

VIDSCOPE_TEST(Nonzero_stream_start_is_normalized_for_duration_seek_and_stepping)
{
    const auto path = fixtureDirectory / "nonzero_start.mkv";
    const auto reference = decodeAll(path);
    VIDSCOPE_REQUIRE(reference.size() == 5);
    VIDSCOPE_REQUIRE(reference.front()->presentationTime == 0ns);
    VIDSCOPE_REQUIRE(reference.back()->presentationTime == 800ms);

    vidscope::playback::PlaybackSession session(testConfig());
    const auto opened = session.open(path);
    VIDSCOPE_REQUIRE(static_cast<bool>(opened));
    const auto* info = session.mediaInfo();
    VIDSCOPE_REQUIRE(info != nullptr);
    VIDSCOPE_REQUIRE(info->streamStartTimestamp > 0);
    VIDSCOPE_REQUIRE(info->duration == 1s);
    VIDSCOPE_REQUIRE(opened.frame->presentationTime == 0ns);

    const auto sought = session.seek(
        {50, 350ms, vidscope::playback::SeekBias::AtOrAfter});
    VIDSCOPE_REQUIRE(static_cast<bool>(sought));
    requireSameFrameContent(*sought.frame, *reference[2]);

    const auto previous = session.previousFrame();
    VIDSCOPE_REQUIRE(static_cast<bool>(previous));
    requireSameFrameContent(*previous.frame, *reference[1]);

    const auto restored = session.nextFrame();
    VIDSCOPE_REQUIRE(static_cast<bool>(restored));
    requireSameFrameContent(*restored.frame, *reference[2]);

    const auto final = session.seek(
        {51, info->duration, vidscope::playback::SeekBias::AtOrAfter});
    VIDSCOPE_REQUIRE(static_cast<bool>(final));
    requireSameFrameContent(*final.frame, *reference.back());
}

VIDSCOPE_TEST(Duplicate_PTS_reverse_reconstruction_uses_exact_visible_content)
{
    auto config = testConfig();
    config.frameCacheBytes = 1;
    config.forwardQueueBytes = 1;
    config.forwardQueueFrames = 1;
    config.initialPrefetchFrames = 0;
    config.presentationIndexAnchorCount = 0;

    vidscope::playback::PlaybackSession session(config);
    const auto firstAtZero = session.open(fixtureDirectory / "duplicate_pts.mkv");
    VIDSCOPE_REQUIRE(static_cast<bool>(firstAtZero));

    const auto secondAtZero = session.nextFrame();
    VIDSCOPE_REQUIRE(static_cast<bool>(secondAtZero));
    VIDSCOPE_REQUIRE(firstAtZero.frame->presentationTime == 0ns);
    VIDSCOPE_REQUIRE(secondAtZero.frame->presentationTime == 0ns);
    VIDSCOPE_REQUIRE(!vidscope::media::visibleImagesEqual(
        *firstAtZero.frame, *secondAtZero.frame));

    const auto previousAtZero = session.previousFrame();
    VIDSCOPE_REQUIRE(static_cast<bool>(previousAtZero));
    VIDSCOPE_REQUIRE(vidscope::media::visibleImagesEqual(
        *previousAtZero.frame, *firstAtZero.frame));

    const auto restoredAtZero = session.nextFrame();
    VIDSCOPE_REQUIRE(static_cast<bool>(restoredAtZero));
    VIDSCOPE_REQUIRE(vidscope::media::visibleImagesEqual(
        *restoredAtZero.frame, *secondAtZero.frame));

    const auto firstLater = session.nextFrame();
    VIDSCOPE_REQUIRE(static_cast<bool>(firstLater));
    const auto secondLater = session.nextFrame();
    VIDSCOPE_REQUIRE(static_cast<bool>(secondLater));
    VIDSCOPE_REQUIRE(firstLater.frame->presentationTime == secondLater.frame->presentationTime);
    VIDSCOPE_REQUIRE(firstLater.frame->presentationTime > 0ns);
    VIDSCOPE_REQUIRE(!vidscope::media::visibleImagesEqual(
        *firstLater.frame, *secondLater.frame));

    const auto previousLater = session.previousFrame();
    VIDSCOPE_REQUIRE(static_cast<bool>(previousLater));
    VIDSCOPE_REQUIRE(vidscope::media::visibleImagesEqual(
        *previousLater.frame, *firstLater.frame));

    const auto restoredLater = session.nextFrame();
    VIDSCOPE_REQUIRE(static_cast<bool>(restoredLater));
    VIDSCOPE_REQUIRE(vidscope::media::visibleImagesEqual(
        *restoredLater.frame, *secondLater.frame));
}

VIDSCOPE_TEST(Default_hardware_policy_decodes_or_falls_back_and_closes_cleanly)
{
    vidscope::playback::PlaybackSession session;
    const auto opened = session.open(fixtureDirectory / "cfr_bframes.mp4");
    VIDSCOPE_REQUIRE(static_cast<bool>(opened));

    std::size_t frameCount = 1;
    auto previousTime = opened.frame->presentationTime;
    for (;;) {
        const auto next = session.nextFrame();
        if (next.status == vidscope::playback::NavigationStatus::EndOfStream) {
            break;
        }
        VIDSCOPE_REQUIRE(static_cast<bool>(next));
        VIDSCOPE_REQUIRE(next.frame->presentationTime > previousTime);
        previousTime = next.frame->presentationTime;
        ++frameCount;
    }
    VIDSCOPE_REQUIRE(frameCount == 24);

    session.close();
    VIDSCOPE_REQUIRE(!session.isOpen());
}

VIDSCOPE_TEST(Random_seek_then_previous_and_next_is_exact_for_CFR_with_B_frames)
{
    verifySeekAndReverse("cfr_bframes.mp4", 1'050ms);
}

VIDSCOPE_TEST(Previous_frame_reconstruction_crosses_a_long_GOP_correctly)
{
    verifySeekAndReverse("long_gop.mp4", 3'050ms);
}

VIDSCOPE_TEST(Cancelled_seek_never_publishes_a_stale_frame)
{
    vidscope::playback::PlaybackSession session(testConfig());
    VIDSCOPE_REQUIRE(static_cast<bool>(session.open(fixtureDirectory / "long_gop.mp4")));
    vidscope::core::CancellationSource cancellation;
    cancellation.requestCancellation();
    const auto result = session.seek(
        {99, 5s, vidscope::playback::SeekBias::AtOrAfter}, cancellation.token());
    VIDSCOPE_REQUIRE(result.status == vidscope::playback::NavigationStatus::Cancelled);
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        return 2;
    }
    fixtureDirectory = std::filesystem::path(argv[1]);
    return vidscope::test::runAll();
}




