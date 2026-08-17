#include "TestHarness.h"

#include "timeline/TimelineModel.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace std::chrono_literals;

namespace {

vidscope::timeline::FrameBoundary boundary(
    const std::int64_t presentationIndex,
    const vidscope::media::MediaTime time,
    const vidscope::media::MediaTime duration,
    const std::uint64_t serial = 0,
    const bool keyFrame = false)
{
    return {
        vidscope::media::FrameId{presentationIndex, presentationIndex * 10, serial},
        time,
        duration,
        keyFrame,
    };
}

} // namespace

VIDSCOPE_TEST(TimelineModel_maps_nanoseconds_after_a_long_absolute_offset)
{
    vidscope::timeline::TimelineModel model;
    const auto start = std::chrono::duration_cast<vidscope::media::MediaTime>(
                           std::chrono::hours{24 * 100})
        + 123ns;
    model.reset(std::chrono::duration_cast<vidscope::media::MediaTime>(
        std::chrono::hours{24 * 200}));
    VIDSCOPE_REQUIRE(model.setViewport(start, start + 1s));

    const double oneNanosecondPixel = model.timeToPixel(start + 1ns, 0.0, 1'000'000'000.0);
    VIDSCOPE_REQUIRE(std::abs(oneNanosecondPixel - 1.0) < 1e-9);
    VIDSCOPE_REQUIRE(model.pixelToTime(1.0, 0.0, 1'000'000'000.0) == start + 1ns);
    VIDSCOPE_REQUIRE(model.pixelToTime(-1.0, 0.0, 1'000'000'000.0) == start);
    VIDSCOPE_REQUIRE(model.pixelToTime(1'000'000'001.0, 0.0, 1'000'000'000.0)
        == start + 1s);
}

VIDSCOPE_TEST(TimelineModel_clamps_and_normalizes_viewport_playhead_pan_and_selection)
{
    vidscope::timeline::TimelineModel model;
    model.reset(10s);

    VIDSCOPE_REQUIRE(model.setViewport(9s, 2s));
    VIDSCOPE_REQUIRE(model.viewportStart() == 2s);
    VIDSCOPE_REQUIRE(model.viewportEnd() == 9s);
    VIDSCOPE_REQUIRE(model.setViewport(-5s, 20s));
    VIDSCOPE_REQUIRE(model.isShowingEntireMedia());

    VIDSCOPE_REQUIRE(model.setViewport(10s, 10s));
    VIDSCOPE_REQUIRE(model.viewportEnd() == 10s);
    VIDSCOPE_REQUIRE(model.visibleDuration() == vidscope::timeline::TimelineModel::kMinimumViewportDuration);

    VIDSCOPE_REQUIRE(model.setPlayhead(20s));
    VIDSCOPE_REQUIRE(model.playhead() == 10s);
    VIDSCOPE_REQUIRE(model.panBy(-20s));
    VIDSCOPE_REQUIRE(model.viewportStart() == 0ns);
    VIDSCOPE_REQUIRE(!model.panBy(-1ns));

    VIDSCOPE_REQUIRE(model.setSelection(12s, -3s));
    VIDSCOPE_REQUIRE(model.selection().has_value());
    VIDSCOPE_REQUIRE(model.selection()->start == 0ns);
    VIDSCOPE_REQUIRE(model.selection()->end == 10s);
}

VIDSCOPE_TEST(TimelineModel_zoom_preserves_anchor_and_ticks_use_1_2_5_intervals)
{
    vidscope::timeline::TimelineModel model;
    model.reset(100s);
    VIDSCOPE_REQUIRE(model.setViewport(20s, 60s));

    const auto anchor = 30s;
    const double before = model.timeToPixel(anchor, 0.0, 800.0);
    VIDSCOPE_REQUIRE(model.zoomAt(2.0, anchor));
    VIDSCOPE_REQUIRE(model.viewportStart() == 25s);
    VIDSCOPE_REQUIRE(model.viewportEnd() == 45s);
    VIDSCOPE_REQUIRE(std::abs(model.timeToPixel(anchor, 0.0, 800.0) - before) < 1e-9);

    VIDSCOPE_REQUIRE(model.setViewport(0s, 9s));
    VIDSCOPE_REQUIRE(model.majorTickInterval(900.0, 90.0) == 1s);
    VIDSCOPE_REQUIRE(model.setViewport(0s, 18s));
    VIDSCOPE_REQUIRE(model.majorTickInterval(900.0, 90.0) == 2s);
    VIDSCOPE_REQUIRE(model.setViewport(0s, 45s));
    VIDSCOPE_REQUIRE(model.majorTickInterval(900.0, 90.0) == 5s);

    VIDSCOPE_REQUIRE(model.setViewport(1s, 5s));
    const auto ticks = model.majorTicks(400.0, 90.0);
    VIDSCOPE_REQUIRE(ticks.size() == 5);
    VIDSCOPE_REQUIRE(ticks.front() == 1s);
    VIDSCOPE_REQUIRE(ticks.back() == 5s);
}

VIDSCOPE_TEST(TimelineModel_preserves_exact_VFR_boundaries_in_presentation_order)
{
    vidscope::timeline::TimelineModel model;
    model.reset(1s);

    VIDSCOPE_REQUIRE(model.observeFrame(boundary(2, 100ms, 25ms, 12)));
    VIDSCOPE_REQUIRE(model.observeFrame(boundary(0, 0ms, 40ms, 10, true)));
    VIDSCOPE_REQUIRE(model.observeFrame(boundary(1, 40ms, 60ms, 11)));
    VIDSCOPE_REQUIRE(model.observeFrame(boundary(3, 100ms, 75ms, 13)));

    const auto frames = model.knownFrames();
    VIDSCOPE_REQUIRE(frames.size() == 4);
    VIDSCOPE_REQUIRE(frames[0].id.presentationIndex == 0);
    VIDSCOPE_REQUIRE(frames[0].duration == 40ms);
    VIDSCOPE_REQUIRE(frames[1].id.presentationIndex == 1);
    VIDSCOPE_REQUIRE(frames[1].duration == 60ms);
    VIDSCOPE_REQUIRE(frames[2].id.presentationIndex == 2);
    VIDSCOPE_REQUIRE(frames[2].time == 100ms);
    VIDSCOPE_REQUIRE(frames[3].id.presentationIndex == 3);
    VIDSCOPE_REQUIRE(frames[3].time == 100ms);

    const auto replacement = boundary(1, 50ms, 50ms, 11);
    VIDSCOPE_REQUIRE(model.observeFrame(replacement));
    VIDSCOPE_REQUIRE(model.knownFrameCount() == 4);
    VIDSCOPE_REQUIRE(model.knownFrames()[1] == replacement);
    VIDSCOPE_REQUIRE(!model.observeFrame(replacement));
}

VIDSCOPE_TEST(TimelineModel_enforces_hard_frame_and_marker_capacity)
{
    vidscope::timeline::TimelineModel model(2, 2);
    model.reset(10s);

    VIDSCOPE_REQUIRE(model.observeFrame(boundary(0, 0s, 1s, 1)));
    VIDSCOPE_REQUIRE(model.observeFrame(boundary(1, 1s, 1s, 2)));
    model.setPlayhead(2s);
    VIDSCOPE_REQUIRE(model.observeFrame(boundary(2, 2s, 1s, 3)));
    VIDSCOPE_REQUIRE(model.knownFrameCount() == 2);
    VIDSCOPE_REQUIRE(model.knownFrameCount() <= model.maximumKnownFrames());

    const auto first = model.addMarker(8s, vidscope::timeline::TimelineMarkerKind::Bookmark);
    const auto second = model.addMarker(2s, vidscope::timeline::TimelineMarkerKind::Chapter);
    VIDSCOPE_REQUIRE(first.has_value());
    VIDSCOPE_REQUIRE(second.has_value());
    VIDSCOPE_REQUIRE(!model.addMarker(5s, vidscope::timeline::TimelineMarkerKind::Scene).has_value());
    VIDSCOPE_REQUIRE(model.markers().size() == 2);
}

VIDSCOPE_TEST(TimelineModel_visible_queries_are_closed_at_both_viewport_edges)
{
    vidscope::timeline::TimelineModel model;
    model.reset(10s);
    VIDSCOPE_REQUIRE(model.observeFrame(boundary(0, 1s, 1s, 1)));
    VIDSCOPE_REQUIRE(model.observeFrame(boundary(1, 2s, 1s, 2)));
    VIDSCOPE_REQUIRE(model.observeFrame(boundary(2, 3s, 1s, 3)));
    VIDSCOPE_REQUIRE(model.observeFrame(boundary(3, 4s, 1s, 4)));

    const auto before = model.addMarker(1s, vidscope::timeline::TimelineMarkerKind::Bookmark);
    const auto atStart = model.addMarker(2s, vidscope::timeline::TimelineMarkerKind::Bookmark);
    const auto atEnd = model.addMarker(3s, vidscope::timeline::TimelineMarkerKind::Bookmark);
    const auto after = model.addMarker(4s, vidscope::timeline::TimelineMarkerKind::Bookmark);
    VIDSCOPE_REQUIRE(before && atStart && atEnd && after);

    VIDSCOPE_REQUIRE(model.setViewport(2s, 3s));
    const auto visibleFrames = model.visibleFrameBoundaries(1'000.0, 1.0);
    VIDSCOPE_REQUIRE(visibleFrames.size() == 2);
    VIDSCOPE_REQUIRE(visibleFrames.front().time == 2s);
    VIDSCOPE_REQUIRE(visibleFrames.back().time == 3s);

    const auto visibleMarkers = model.visibleMarkers();
    VIDSCOPE_REQUIRE(visibleMarkers.size() == 2);
    VIDSCOPE_REQUIRE(visibleMarkers.front().id == *atStart);
    VIDSCOPE_REQUIRE(visibleMarkers.back().id == *atEnd);
}

VIDSCOPE_TEST(TimelineModel_marker_ids_survive_sorting_updates_and_removal)
{
    vidscope::timeline::TimelineModel model;
    model.reset(10s);

    const auto first = model.addMarker(
        8s, vidscope::timeline::TimelineMarkerKind::Bookmark, QStringLiteral("first"));
    const auto second = model.addMarker(
        2s, vidscope::timeline::TimelineMarkerKind::Scene, QStringLiteral("second"));
    VIDSCOPE_REQUIRE(first && second);
    VIDSCOPE_REQUIRE(model.markers()[0].id == *second);
    VIDSCOPE_REQUIRE(model.markers()[1].id == *first);

    VIDSCOPE_REQUIRE(model.updateMarker(
        *first, 1s, vidscope::timeline::TimelineMarkerKind::Chapter, QStringLiteral("moved")));
    VIDSCOPE_REQUIRE(model.markers()[0].id == *first);
    VIDSCOPE_REQUIRE(model.markers()[0].label == QStringLiteral("moved"));
    VIDSCOPE_REQUIRE(model.removeMarker(*second));

    const auto third = model.addMarker(3s, vidscope::timeline::TimelineMarkerKind::Bookmark);
    VIDSCOPE_REQUIRE(third && *third > *second);
    model.reset(10s);
    const auto afterReset = model.addMarker(4s, vidscope::timeline::TimelineMarkerKind::Bookmark);
    VIDSCOPE_REQUIRE(afterReset && *afterReset > *third);
}

VIDSCOPE_TEST(TimelineModel_selection_details_use_only_observed_presentation_identity)
{
    vidscope::timeline::TimelineModel model;
    model.reset(1s);
    VIDSCOPE_REQUIRE(model.observeFrame(boundary(10, 10ms, 17ms, 10)));
    VIDSCOPE_REQUIRE(model.observeFrame(boundary(11, 27ms, 31ms, 11)));
    VIDSCOPE_REQUIRE(model.observeFrame(boundary(12, 58ms, 9ms, 12)));

    VIDSCOPE_REQUIRE(model.setSelection(58ms, 10ms));
    const auto details = model.selectionDetails();
    VIDSCOPE_REQUIRE(details.range.start == 10ms);
    VIDSCOPE_REQUIRE(details.range.end == 58ms);
    VIDSCOPE_REQUIRE(details.knownFrameCount == 3);
    VIDSCOPE_REQUIRE(details.firstFrame.has_value());
    VIDSCOPE_REQUIRE(details.firstFrame->presentationIndex == 10);
    VIDSCOPE_REQUIRE(details.lastFrame.has_value());
    VIDSCOPE_REQUIRE(details.lastFrame->presentationIndex == 12);
    VIDSCOPE_REQUIRE(details.frameCount == 3);

    VIDSCOPE_REQUIRE(model.setSelection(0ms, 58ms));
    const auto partialStart = model.selectionDetails();
    VIDSCOPE_REQUIRE(partialStart.knownFrameCount == 3);
    VIDSCOPE_REQUIRE(!partialStart.frameCount.has_value());

    VIDSCOPE_REQUIRE(model.setSelection(10ms, 60ms));
    const auto partialEnd = model.selectionDetails();
    VIDSCOPE_REQUIRE(partialEnd.knownFrameCount == 3);
    VIDSCOPE_REQUIRE(!partialEnd.frameCount.has_value());

    VIDSCOPE_REQUIRE(model.setSelection(10ms, 58ms));
    VIDSCOPE_REQUIRE(model.selectionDetails().frameCount == 3);
    VIDSCOPE_REQUIRE(model.observeFrame(boundary(11, 70ms, 31ms, 99)));
    const auto afterFrameMutation = model.selectionDetails();
    VIDSCOPE_REQUIRE(afterFrameMutation.knownFrameCount == 2);
    VIDSCOPE_REQUIRE(!afterFrameMutation.frameCount.has_value());

    vidscope::timeline::TimelineModel sparse;
    sparse.reset(1s);
    VIDSCOPE_REQUIRE(sparse.observeFrame(boundary(10, 10ms, 17ms, 20)));
    VIDSCOPE_REQUIRE(sparse.observeFrame(boundary(12, 58ms, 9ms, 22)));
    VIDSCOPE_REQUIRE(sparse.setSelection(10ms, 58ms));
    const auto sparseDetails = sparse.selectionDetails();
    VIDSCOPE_REQUIRE(sparseDetails.knownFrameCount == 2);
    VIDSCOPE_REQUIRE(!sparseDetails.frameCount.has_value());
}




VIDSCOPE_TEST(TimelineModel_upserts_redecoded_serials_by_presentation_identity)
{
    vidscope::timeline::TimelineModel model;
    model.reset(10s);

    const auto original = boundary(7, 2s, 1s, 101);
    const auto redecoded = boundary(7, 4s, 500ms, 202, true);
    VIDSCOPE_REQUIRE(model.observeFrame(original));

    const auto originalPixel = model.frameToPixel(original.id, 0.0, 1'000.0);
    VIDSCOPE_REQUIRE(originalPixel.has_value());
    VIDSCOPE_REQUIRE(std::abs(*originalPixel - 200.0) < 1e-9);

    VIDSCOPE_REQUIRE(model.observeFrame(redecoded));
    VIDSCOPE_REQUIRE(model.knownFrameCount() == 1);
    VIDSCOPE_REQUIRE(model.knownFrames().front() == redecoded);

    const auto oldSerialPixel = model.frameToPixel(original.id, 0.0, 1'000.0);
    const auto newSerialPixel = model.frameToPixel(redecoded.id, 0.0, 1'000.0);
    VIDSCOPE_REQUIRE(oldSerialPixel.has_value());
    VIDSCOPE_REQUIRE(newSerialPixel.has_value());
    VIDSCOPE_REQUIRE(std::abs(*oldSerialPixel - 400.0) < 1e-9);
    VIDSCOPE_REQUIRE(*oldSerialPixel == *newSerialPixel);
    VIDSCOPE_REQUIRE(!model.observeFrame(redecoded));

    const vidscope::timeline::FrameBoundary unknownFirst{
        vidscope::media::FrameId{-1, 90, 1},
        5s,
        1s,
        false,
    };
    const vidscope::timeline::FrameBoundary unknownSecond{
        vidscope::media::FrameId{-1, 90, 2},
        5s,
        1s,
        false,
    };
    VIDSCOPE_REQUIRE(model.observeFrame(unknownFirst));
    VIDSCOPE_REQUIRE(model.observeFrame(unknownSecond));
    VIDSCOPE_REQUIRE(model.knownFrameCount() == 3);
    VIDSCOPE_REQUIRE(model.frameToPixel(unknownFirst.id, 0.0, 1'000.0).has_value());
    VIDSCOPE_REQUIRE(model.frameToPixel(unknownSecond.id, 0.0, 1'000.0).has_value());
}

VIDSCOPE_TEST(TimelineModel_retains_and_renders_equal_time_collision_groups)
{
    vidscope::timeline::TimelineModel model;
    model.reset(3s);

    VIDSCOPE_REQUIRE(model.observeFrame(boundary(2, 1s, 1s, 3)));
    VIDSCOPE_REQUIRE(model.observeFrame(boundary(0, 1s, 1s, 1)));
    VIDSCOPE_REQUIRE(model.observeFrame(boundary(1, 1s, 1s, 2)));
    VIDSCOPE_REQUIRE(model.observeFrame(boundary(3, 2s, 1s, 4)));
    VIDSCOPE_REQUIRE(model.setViewport(1s, 2s));

    const auto visible = model.visibleFrameBoundaries(100.0, 100.0);
    VIDSCOPE_REQUIRE(visible.size() == 4);
    for (std::size_t index = 0; index < visible.size(); ++index) {
        VIDSCOPE_REQUIRE(
            visible[index].id.presentationIndex == static_cast<std::int64_t>(index));
    }
    VIDSCOPE_REQUIRE(visible[0].time == visible[1].time);
    VIDSCOPE_REQUIRE(visible[1].time == visible[2].time);

    VIDSCOPE_REQUIRE(model.visibleFrameBoundaries(100.0, 100.001).empty());
    VIDSCOPE_REQUIRE(model.visibleFrameBoundaries(100.0, 0.0, 3).empty());
}

VIDSCOPE_TEST(TimelineModel_reorders_conflicting_index_time_and_pts_updates)
{
    vidscope::timeline::TimelineModel model;
    model.reset(10s);

    const vidscope::timeline::FrameBoundary original{
        vidscope::media::FrameId{7, 700, 1},
        5s,
        1s,
        false,
    };
    const vidscope::timeline::FrameBoundary before{
        vidscope::media::FrameId{6, 9'000, 2},
        2s,
        1s,
        false,
    };
    const vidscope::timeline::FrameBoundary after{
        vidscope::media::FrameId{8, -900, 3},
        2s,
        1s,
        false,
    };
    const vidscope::timeline::FrameBoundary replacement{
        vidscope::media::FrameId{7, -7'000, 99},
        2s,
        500ms,
        true,
    };

    VIDSCOPE_REQUIRE(model.observeFrame(original));
    VIDSCOPE_REQUIRE(model.observeFrame(after));
    VIDSCOPE_REQUIRE(model.observeFrame(before));
    VIDSCOPE_REQUIRE(model.observeFrame(replacement));

    const auto frames = model.knownFrames();
    VIDSCOPE_REQUIRE(frames.size() == 3);
    VIDSCOPE_REQUIRE(frames[0] == before);
    VIDSCOPE_REQUIRE(frames[1] == replacement);
    VIDSCOPE_REQUIRE(frames[2] == after);
}

VIDSCOPE_TEST(TimelineModel_eviction_tracks_updated_identity_and_prefers_front_on_ties)
{
    vidscope::timeline::TimelineModel model(3, 0);
    model.reset(10s);

    const auto atOne = boundary(0, 1s, 1s, 1);
    const auto atFour = boundary(1, 4s, 1s, 2);
    const auto atSix = boundary(2, 6s, 1s, 3);
    const auto atNine = boundary(3, 9s, 1s, 4);
    VIDSCOPE_REQUIRE(model.observeFrame(atOne));
    VIDSCOPE_REQUIRE(model.observeFrame(atFour));
    VIDSCOPE_REQUIRE(model.observeFrame(atSix));
    VIDSCOPE_REQUIRE(model.setPlayhead(5s));

    VIDSCOPE_REQUIRE(model.observeFrame(atNine));
    VIDSCOPE_REQUIRE(model.knownFrames().front() == atFour);
    VIDSCOPE_REQUIRE(model.knownFrames().back() == atNine);
    VIDSCOPE_REQUIRE(!model.frameToPixel(atOne.id, 0.0, 100.0).has_value());

    const auto rejectedFarFrame = boundary(4, 0s, 1s, 5);
    VIDSCOPE_REQUIRE(!model.observeFrame(rejectedFarFrame));
    VIDSCOPE_REQUIRE(model.knownFrames().front() == atFour);

    const auto updatedAtZero = boundary(2, 0s, 1s, 99);
    VIDSCOPE_REQUIRE(model.observeFrame(updatedAtZero));
    VIDSCOPE_REQUIRE(model.knownFrames().front() == updatedAtZero);
    const auto oldSerialPixel = model.frameToPixel(atSix.id, 0.0, 100.0);
    VIDSCOPE_REQUIRE(oldSerialPixel.has_value());
    VIDSCOPE_REQUIRE(*oldSerialPixel == 0.0);

    const auto atPlayhead = boundary(5, 5s, 1s, 6);
    VIDSCOPE_REQUIRE(model.observeFrame(atPlayhead));
    VIDSCOPE_REQUIRE(model.knownFrameCount() == 3);
    VIDSCOPE_REQUIRE(model.knownFrames()[0] == atFour);
    VIDSCOPE_REQUIRE(model.knownFrames()[1] == atPlayhead);
    VIDSCOPE_REQUIRE(model.knownFrames()[2] == atNine);
    VIDSCOPE_REQUIRE(!model.frameToPixel(updatedAtZero.id, 0.0, 100.0).has_value());
    VIDSCOPE_REQUIRE(!model.frameToPixel(atSix.id, 0.0, 100.0).has_value());
}

VIDSCOPE_TEST(TimelineModel_zoom_anchor_scaling_is_exact_beyond_double_integer_range)
{
    using vidscope::media::MediaTime;

    constexpr std::int64_t fullDuration = std::int64_t{1} << 54;
    constexpr std::int64_t halfDuration = std::int64_t{1} << 53;
    constexpr std::int64_t anchorValue = 3 * (std::int64_t{1} << 52) + 1;
    constexpr std::int64_t expectedStart = 3 * (std::int64_t{1} << 51);

    vidscope::timeline::TimelineModel model;
    model.reset(MediaTime{fullDuration});

    const auto largeOffset = MediaTime{halfDuration + 123};
    VIDSCOPE_REQUIRE(model.setViewport(largeOffset, largeOffset + MediaTime{4'000}));
    VIDSCOPE_REQUIRE(
        std::abs(model.timeToPixel(largeOffset + MediaTime{1}, 0.0, 4'000.0) - 1.0)
        < 1e-9);

    VIDSCOPE_REQUIRE(model.setViewport(MediaTime::zero(), MediaTime{fullDuration}));
    const auto initialStart = model.viewportStart();
    const auto initialEnd = model.viewportEnd();
    VIDSCOPE_REQUIRE(!model.zoomAt(0.0, MediaTime{anchorValue}));
    VIDSCOPE_REQUIRE(!model.zoomAt(
        std::numeric_limits<double>::infinity(), MediaTime{anchorValue}));
    VIDSCOPE_REQUIRE(!model.zoomAt(
        std::numeric_limits<double>::quiet_NaN(), MediaTime{anchorValue}));
    VIDSCOPE_REQUIRE(model.viewportStart() == initialStart);
    VIDSCOPE_REQUIRE(model.viewportEnd() == initialEnd);

    VIDSCOPE_REQUIRE(model.zoomAt(2.0, MediaTime{anchorValue}));
    VIDSCOPE_REQUIRE(model.viewportStart() == MediaTime{expectedStart});
    VIDSCOPE_REQUIRE(
        model.viewportEnd() == MediaTime{expectedStart + halfDuration});
    VIDSCOPE_REQUIRE(model.pixelToTime(
        -std::numeric_limits<double>::infinity(), 0.0, 1'000.0)
        == model.viewportStart());
    VIDSCOPE_REQUIRE(model.pixelToTime(
        std::numeric_limits<double>::infinity(), 0.0, 1'000.0)
        == model.viewportEnd());
    VIDSCOPE_REQUIRE(model.pixelToTime(
        std::numeric_limits<double>::quiet_NaN(), 0.0, 1'000.0)
        == model.viewportStart());
}
