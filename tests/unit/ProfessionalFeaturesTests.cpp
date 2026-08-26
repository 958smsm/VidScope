#include "TestHarness.h"

#include "analysis/VisualSearch.h"
#include "inspection/FrameHistory.h"
#include "timeline/TimelineModel.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

using namespace std::chrono_literals;

namespace {

vidscope::media::DecodedFrame frame(
    const std::int64_t index,
    const vidscope::media::MediaTime time)
{
    vidscope::media::DecodedFrame result;
    result.id = {index, index * 10, static_cast<std::uint64_t>(index + 1)};
    result.presentationTime = time;
    result.duration = 40ms;
    result.pictureType = index == 0 ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_P;
    result.keyFrame = index == 0;
    return result;
}

vidscope::analysis::AnalysisSample sample(
    const std::int64_t index,
    const vidscope::media::MediaTime time,
    const std::optional<std::uint64_t> hash)
{
    vidscope::analysis::AnalysisSample result;
    result.presentationIndex = index;
    result.presentationTime = time;
    result.perceptualHash = hash;
    return result;
}

} // namespace

VIDSCOPE_TEST(FrameHistory_is_bounded_and_discards_a_stale_forward_branch)
{
    vidscope::inspection::FrameHistory history(3);
    const auto first = frame(0, 0ms);
    const auto second = frame(1, 40ms);
    const auto third = frame(2, 80ms);
    const auto fourth = frame(3, 120ms);
    VIDSCOPE_REQUIRE(history.visit(first));
    VIDSCOPE_REQUIRE(history.visit(second));
    VIDSCOPE_REQUIRE(history.visit(third));
    VIDSCOPE_REQUIRE(history.entries().size() == 3);
    VIDSCOPE_REQUIRE(history.back()->id.presentationIndex == 1);
    VIDSCOPE_REQUIRE(history.canGoForward());
    VIDSCOPE_REQUIRE(history.visit(fourth));
    VIDSCOPE_REQUIRE(!history.canGoForward());
    VIDSCOPE_REQUIRE(history.entries().size() == 3);
    VIDSCOPE_REQUIRE(history.entries().back().id.presentationIndex == 3);

    const auto duplicate = fourth;
    VIDSCOPE_REQUIRE(!history.visit(duplicate));
    VIDSCOPE_REQUIRE(history.entries().size() == 3);
}

VIDSCOPE_TEST(VisualSearch_ranks_by_hamming_then_temporal_distance)
{
    const std::vector<vidscope::analysis::AnalysisSample> samples{
        sample(0, 0ms, 0x00ULL),
        sample(1, 1s, 0x03ULL),
        sample(2, 2s, 0x01ULL),
        sample(3, 3s, std::nullopt),
        sample(4, 4s, 0xffffffffffffffffULL),
    };
    const auto matches = vidscope::analysis::VisualSearch::findSimilar(
        samples,
        0x00ULL,
        0ms,
        0,
        8,
        8);
    VIDSCOPE_REQUIRE(matches.size() == 2);
    VIDSCOPE_REQUIRE(matches[0].presentationIndex == 2);
    VIDSCOPE_REQUIRE(matches[0].hammingDistance == 1);
    VIDSCOPE_REQUIRE(matches[1].presentationIndex == 1);
    VIDSCOPE_REQUIRE(matches[1].hammingDistance == 2);
}

VIDSCOPE_TEST(Timeline_markers_preserve_professional_category_and_note_metadata)
{
    vidscope::timeline::TimelineModel timeline;
    timeline.reset(10s);
    const auto id = timeline.addMarker(
        3s,
        vidscope::timeline::TimelineMarkerKind::Bookmark,
        QStringLiteral("Continuity"),
        QStringLiteral("Issue"),
        QStringLiteral("Check the cut."));
    VIDSCOPE_REQUIRE(id.has_value());
    VIDSCOPE_REQUIRE(timeline.markers().front().category == QStringLiteral("Issue"));
    VIDSCOPE_REQUIRE(
        timeline.markers().front().note == QStringLiteral("Check the cut."));

    VIDSCOPE_REQUIRE(timeline.updateMarker(
        *id,
        2s,
        vidscope::timeline::TimelineMarkerKind::Chapter,
        QStringLiteral("Revised"),
        QStringLiteral("Review"),
        QStringLiteral("Approved.")));
    VIDSCOPE_REQUIRE(timeline.markers().front().id == *id);
    VIDSCOPE_REQUIRE(timeline.markers().front().category == QStringLiteral("Review"));
    VIDSCOPE_REQUIRE(timeline.markers().front().note == QStringLiteral("Approved."));
}
