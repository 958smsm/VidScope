#include "TestHarness.h"

#include "playback/PlaybackSession.h"
#include "timeline/TimelineModel.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

using vidscope::media::DecodedFramePtr;
using vidscope::media::MediaTime;
using vidscope::playback::NavigationStatus;
using vidscope::timeline::FrameBoundary;
using vidscope::timeline::TimelineModel;

std::filesystem::path fixtureDirectory;

struct DecodedFixture final {
    MediaTime duration{};
    std::vector<DecodedFramePtr> frames;
};

vidscope::playback::PlaybackSessionConfig softwareDecodeConfig()
{
    vidscope::playback::PlaybackSessionConfig config;
    config.frameCacheBytes = 64U * 1024U * 1024U;
    config.forwardQueueBytes = 32U * 1024U * 1024U;
    config.forwardQueueFrames = 12;
    config.initialPrefetchFrames = 6;
    config.decoder.hardwareAcceleration = vidscope::media::HardwareAcceleration::Disabled;
    return config;
}

DecodedFixture decodeEveryFrame(const char* fixtureName)
{
    const auto path = fixtureDirectory / fixtureName;
    vidscope::playback::PlaybackSession session(softwareDecodeConfig());
    const auto opened = session.open(path);
    VIDSCOPE_REQUIRE_MESSAGE(static_cast<bool>(opened), path.string());
    VIDSCOPE_REQUIRE(opened.frame != nullptr);
    VIDSCOPE_REQUIRE(session.mediaInfo() != nullptr);

    DecodedFixture decoded;
    decoded.duration = session.mediaInfo()->duration;
    decoded.frames.push_back(opened.frame);

    for (std::size_t guard = 0; guard < 10'000; ++guard) {
        const auto next = session.nextFrame();
        if (next.status == NavigationStatus::EndOfStream) {
            session.close();
            VIDSCOPE_REQUIRE(!session.isOpen());
            return decoded;
        }
        VIDSCOPE_REQUIRE(next.status == NavigationStatus::FrameReady);
        VIDSCOPE_REQUIRE(next.frame != nullptr);
        VIDSCOPE_REQUIRE(
            next.frame->presentationTime > decoded.frames.back()->presentationTime);
        decoded.frames.push_back(next.frame);
    }

    vidscope::test::fail(
        "PlaybackSession reaches end of stream",
        __FILE__,
        __LINE__,
        path.string());
}

void observeInDecodedOrder(TimelineModel& model, const DecodedFixture& decoded)
{
    model.reset(decoded.duration);
    for (const auto& frame : decoded.frames) {
        VIDSCOPE_REQUIRE(frame != nullptr);
        VIDSCOPE_REQUIRE(model.observeFrame(*frame));
    }
}

void requireExactStoredBoundaries(
    const TimelineModel& model,
    const DecodedFixture& decoded)
{
    const auto boundaries = model.knownFrames();
    VIDSCOPE_REQUIRE(boundaries.size() == decoded.frames.size());
    for (std::size_t index = 0; index < decoded.frames.size(); ++index) {
        const auto& frame = *decoded.frames[index];
        const auto& boundary = boundaries[index];
        VIDSCOPE_REQUIRE(boundary.id == frame.id);
        VIDSCOPE_REQUIRE(boundary.time == frame.presentationTime);
        VIDSCOPE_REQUIRE(boundary.duration == frame.duration);
        VIDSCOPE_REQUIRE(boundary.keyFrame == frame.keyFrame);
    }
}

void requireExactKeyframeData(
    const TimelineModel& model,
    const DecodedFixture& decoded)
{
    std::vector<std::size_t> expected;
    std::vector<std::size_t> actual;
    for (std::size_t index = 0; index < decoded.frames.size(); ++index) {
        if (decoded.frames[index]->keyFrame) {
            expected.push_back(index);
        }
        if (model.knownFrames()[index].keyFrame) {
            actual.push_back(index);
        }
    }

    VIDSCOPE_REQUIRE(!expected.empty());
    VIDSCOPE_REQUIRE(actual == expected);
    for (const auto index : actual) {
        VIDSCOPE_REQUIRE(model.knownFrames()[index].id == decoded.frames[index]->id);
        VIDSCOPE_REQUIRE(
            model.knownFrames()[index].time == decoded.frames[index]->presentationTime);
    }
}

void requireHighZoomTicksAreExactKnownFrames(
    TimelineModel& model,
    const DecodedFixture& decoded,
    std::size_t firstIndex,
    std::size_t lastIndex)
{
    VIDSCOPE_REQUIRE(firstIndex <= lastIndex);
    VIDSCOPE_REQUIRE(lastIndex < decoded.frames.size());
    VIDSCOPE_REQUIRE(model.setViewport(
        decoded.frames[firstIndex]->presentationTime,
        decoded.frames[lastIndex]->presentationTime));

    constexpr double highZoomWidth = 100'000.0;
    const auto visible = model.visibleFrameBoundaries(
        highZoomWidth,
        1.0,
        TimelineModel::kDefaultMaximumKnownFrames);
    VIDSCOPE_REQUIRE(visible.size() == lastIndex - firstIndex + 1);

    std::set<MediaTime> allKnownTimes;
    for (const auto& frame : decoded.frames) {
        allKnownTimes.insert(frame->presentationTime);
    }

    for (std::size_t offset = 0; offset < visible.size(); ++offset) {
        const auto decodedIndex = firstIndex + offset;
        VIDSCOPE_REQUIRE(visible[offset].id == decoded.frames[decodedIndex]->id);
        VIDSCOPE_REQUIRE(
            visible[offset].time == decoded.frames[decodedIndex]->presentationTime);
        VIDSCOPE_REQUIRE(allKnownTimes.contains(visible[offset].time));
    }
}

} // namespace

VIDSCOPE_TEST(TimelineModel_exhaustive_VFR_ingest_preserves_actual_boundaries_and_ticks)
{
    const auto decoded = decodeEveryFrame("vfr.mp4");
    VIDSCOPE_REQUIRE(decoded.frames.size() >= 18);

    TimelineModel model;
    observeInDecodedOrder(model, decoded);
    requireExactStoredBoundaries(model, decoded);
    requireExactKeyframeData(model, decoded);

    std::set<MediaTime::rep> presentationGaps;
    std::set<MediaTime::rep> frameDurations;
    for (std::size_t index = 1; index < decoded.frames.size(); ++index) {
        presentationGaps.insert(
            (decoded.frames[index]->presentationTime
             - decoded.frames[index - 1]->presentationTime)
                .count());
    }
    for (const auto& frame : decoded.frames) {
        VIDSCOPE_REQUIRE(frame->duration > MediaTime::zero());
        frameDurations.insert(frame->duration.count());
    }
    VIDSCOPE_REQUIRE(presentationGaps.size() > 1);
    VIDSCOPE_REQUIRE(frameDurations.size() > 1);

    const std::size_t firstVisible = 2;
    const std::size_t lastVisible = std::min<std::size_t>(10, decoded.frames.size() - 2);
    requireHighZoomTicksAreExactKnownFrames(
        model,
        decoded,
        firstVisible,
        lastVisible);
}

VIDSCOPE_TEST(TimelineModel_B_frame_ingest_orders_by_presentation_and_counts_selection)
{
    const auto decoded = decodeEveryFrame("cfr_bframes.mp4");
    VIDSCOPE_REQUIRE(decoded.frames.size() == 24);
    VIDSCOPE_REQUIRE(std::any_of(
        decoded.frames.cbegin(),
        decoded.frames.cend(),
        [](const auto& frame) { return frame->pictureType == AV_PICTURE_TYPE_B; }));

    TimelineModel model;
    model.reset(decoded.duration);

    // Deliberately observe in reverse order. Timeline order must be rebuilt
    // from presentation time and stable frame identity; insertion/decode/DTS
    // order is never an ordering surrogate.
    for (auto frame = decoded.frames.crbegin(); frame != decoded.frames.crend(); ++frame) {
        VIDSCOPE_REQUIRE(model.observeFrame(**frame));
    }

    requireExactStoredBoundaries(model, decoded);
    requireExactKeyframeData(model, decoded);
    for (std::size_t index = 1; index < model.knownFrames().size(); ++index) {
        const auto& previous = model.knownFrames()[index - 1];
        const auto& current = model.knownFrames()[index];
        VIDSCOPE_REQUIRE(previous.time < current.time);
        VIDSCOPE_REQUIRE(
            previous.id.presentationIndex + 1 == current.id.presentationIndex);
    }

    requireHighZoomTicksAreExactKnownFrames(model, decoded, 4, 12);

    constexpr std::size_t selectionFirst = 3;
    constexpr std::size_t selectionLast = 11;
    VIDSCOPE_REQUIRE(model.setSelection(
        decoded.frames[selectionFirst]->presentationTime,
        decoded.frames[selectionLast]->presentationTime));
    const auto details = model.selectionDetails();
    constexpr auto expectedCount = selectionLast - selectionFirst + 1;
    VIDSCOPE_REQUIRE(details.knownFrameCount == expectedCount);
    VIDSCOPE_REQUIRE(details.frameCount.has_value());
    VIDSCOPE_REQUIRE(*details.frameCount == static_cast<std::int64_t>(expectedCount));
    VIDSCOPE_REQUIRE(details.firstFrame.has_value());
    VIDSCOPE_REQUIRE(details.lastFrame.has_value());
    VIDSCOPE_REQUIRE(*details.firstFrame == decoded.frames[selectionFirst]->id);
    VIDSCOPE_REQUIRE(*details.lastFrame == decoded.frames[selectionLast]->id);
}

VIDSCOPE_TEST(TimelineModel_small_frame_capacity_stays_bounded_under_many_observations)
{
    constexpr std::size_t capacity = 7;
    constexpr std::int64_t observationCount = 25'000;
    TimelineModel model(capacity, 0);
    model.reset(25s);
    VIDSCOPE_REQUIRE(model.setPlayhead(12'500ms));

    for (std::int64_t index = 0; index < observationCount; ++index) {
        FrameBoundary boundary;
        boundary.id = {
            .presentationIndex = index,
            .pts = index * 90,
            .sessionSerial = 1,
        };
        boundary.time = std::chrono::milliseconds(index);
        boundary.duration = 1ms;
        boundary.keyFrame = index % 250 == 0;
        (void)model.observeFrame(boundary);
        VIDSCOPE_REQUIRE(model.knownFrameCount() <= capacity);
    }

    VIDSCOPE_REQUIRE(model.knownFrameCount() == capacity);
    VIDSCOPE_REQUIRE(model.maximumKnownFrames() == capacity);
    const auto retained = model.knownFrames();
    for (std::size_t index = 0; index < retained.size(); ++index) {
        const auto presentationIndex = retained[index].id.presentationIndex;
        VIDSCOPE_REQUIRE(presentationIndex >= 0);
        VIDSCOPE_REQUIRE(presentationIndex < observationCount);
        VIDSCOPE_REQUIRE(retained[index].time == std::chrono::milliseconds(presentationIndex));
        VIDSCOPE_REQUIRE(retained[index].duration == 1ms);
        if (index > 0) {
            VIDSCOPE_REQUIRE(retained[index - 1].time < retained[index].time);
            VIDSCOPE_REQUIRE(
                retained[index - 1].id.presentationIndex
                < retained[index].id.presentationIndex);
        }
    }
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        return 2;
    }
    fixtureDirectory = std::filesystem::path(argv[1]);
    return vidscope::test::runAll();
}
