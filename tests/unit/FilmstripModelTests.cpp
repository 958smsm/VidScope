#include "TestHarness.h"

#include "filmstrip/FilmstripModel.h"

#include <chrono>
#include <cstddef>
#include <cstdint>

using namespace std::chrono_literals;

namespace {

vidscope::timeline::FrameBoundary boundary(
    const std::int64_t presentationIndex,
    const vidscope::media::MediaTime time,
    const vidscope::media::MediaTime duration,
    const std::uint64_t sessionSerial = 1,
    const bool keyFrame = false)
{
    return {
        vidscope::media::FrameId{presentationIndex, presentationIndex * 10, sessionSerial},
        time,
        duration,
        keyFrame,
    };
}

void requireStrictlyIncreasing(const vidscope::filmstrip::FilmstripPlan& plan)
{
    for (std::size_t index = 1; index < plan.targets.size(); ++index) {
        VIDSCOPE_REQUIRE(
            plan.targets[index - 1].requestedTime < plan.targets[index].requestedTime);
    }
}

} // namespace

VIDSCOPE_TEST(FilmstripModel_clamps_custom_counts_to_a_bounded_range)
{
    vidscope::filmstrip::FilmstripModel model;
    VIDSCOPE_REQUIRE(model.count() == vidscope::filmstrip::FilmstripModel::kDefaultCount);

    model.setCount(0);
    VIDSCOPE_REQUIRE(model.count() == vidscope::filmstrip::FilmstripModel::kMinimumCount);

    model.setCount(8);
    VIDSCOPE_REQUIRE(model.count() == 8);
    model.setCount(16);
    VIDSCOPE_REQUIRE(model.count() == 16);
    model.setCount(20);
    VIDSCOPE_REQUIRE(model.count() == 20);
    model.setCount(32);
    VIDSCOPE_REQUIRE(model.count() == 32);

    model.setCount(10'000);
    VIDSCOPE_REQUIRE(model.count() == vidscope::filmstrip::FilmstripModel::kMaximumCount);
}

VIDSCOPE_TEST(FilmstripModel_spreads_entire_video_targets_across_inclusive_endpoints)
{
    vidscope::timeline::TimelineModel timeline;
    timeline.reset(10s);
    VIDSCOPE_REQUIRE(timeline.setPlayhead(4s));

    vidscope::filmstrip::FilmstripModel model;
    model.setCount(8);
    const auto plan = model.makePlan(timeline);

    VIDSCOPE_REQUIRE(plan.status == vidscope::filmstrip::FilmstripPlanStatus::Ready);
    VIDSCOPE_REQUIRE(plan.mode == vidscope::filmstrip::FilmstripMode::EntireVideo);
    VIDSCOPE_REQUIRE(plan.targets.size() == 8);
    VIDSCOPE_REQUIRE(plan.rangeStart == 0ns);
    VIDSCOPE_REQUIRE(plan.rangeEnd == 10s);
    VIDSCOPE_REQUIRE(plan.targets.front().requestedTime == 0ns);
    VIDSCOPE_REQUIRE(plan.targets.back().requestedTime == 10s);
    requireStrictlyIncreasing(plan);

    std::size_t currentCount = 0;
    for (const auto& target : plan.targets) {
        currentCount += target.current ? 1U : 0U;
    }
    VIDSCOPE_REQUIRE(currentCount == 1);
}

VIDSCOPE_TEST(FilmstripModel_uses_visible_timeline_and_selected_ranges_without_fps_estimates)
{
    vidscope::timeline::TimelineModel timeline;
    timeline.reset(20s);
    VIDSCOPE_REQUIRE(timeline.setViewport(3s, 9s));
    VIDSCOPE_REQUIRE(timeline.setPlayhead(5s));

    vidscope::filmstrip::FilmstripModel model;
    model.setCount(5);
    model.setMode(vidscope::filmstrip::FilmstripMode::VisibleTimeline);
    auto plan = model.makePlan(timeline);
    VIDSCOPE_REQUIRE(plan.status == vidscope::filmstrip::FilmstripPlanStatus::Ready);
    VIDSCOPE_REQUIRE(plan.rangeStart == 3s);
    VIDSCOPE_REQUIRE(plan.rangeEnd == 9s);
    VIDSCOPE_REQUIRE(plan.targets.size() == 5);
    VIDSCOPE_REQUIRE(plan.targets.front().requestedTime == 3s);
    VIDSCOPE_REQUIRE(plan.targets.back().requestedTime == 9s);

    model.setMode(vidscope::filmstrip::FilmstripMode::SelectedRange);
    plan = model.makePlan(timeline);
    VIDSCOPE_REQUIRE(
        plan.status == vidscope::filmstrip::FilmstripPlanStatus::SelectionRequired);
    VIDSCOPE_REQUIRE(plan.targets.empty());

    VIDSCOPE_REQUIRE(timeline.setSelection(12s, 7s));
    plan = model.makePlan(timeline);
    VIDSCOPE_REQUIRE(plan.status == vidscope::filmstrip::FilmstripPlanStatus::Ready);
    VIDSCOPE_REQUIRE(plan.rangeStart == 7s);
    VIDSCOPE_REQUIRE(plan.rangeEnd == 12s);
    VIDSCOPE_REQUIRE(plan.targets.front().requestedTime == 7s);
    VIDSCOPE_REQUIRE(plan.targets.back().requestedTime == 12s);
}

VIDSCOPE_TEST(FilmstripModel_around_current_position_prefers_exact_known_frame_boundaries)
{
    vidscope::timeline::TimelineModel timeline;
    timeline.reset(1s);
    constexpr std::uint64_t sessionSerial = 77;
    for (std::int64_t index = 0; index < 12; ++index) {
        VIDSCOPE_REQUIRE(timeline.observeFrame(boundary(
            100 + index,
            std::chrono::milliseconds(index * 20),
            20ms,
            sessionSerial,
            index == 0)));
    }
    VIDSCOPE_REQUIRE(timeline.setPlayhead(100ms));

    vidscope::filmstrip::FilmstripModel model;
    model.setMode(vidscope::filmstrip::FilmstripMode::AroundCurrentPosition);
    model.setCount(8);
    const auto plan = model.makePlan(timeline);

    VIDSCOPE_REQUIRE(plan.status == vidscope::filmstrip::FilmstripPlanStatus::Ready);
    VIDSCOPE_REQUIRE(plan.usesExactContiguousFrames);
    VIDSCOPE_REQUIRE(plan.targets.size() == 8);
    requireStrictlyIncreasing(plan);
    for (std::size_t index = 1; index < plan.targets.size(); ++index) {
        VIDSCOPE_REQUIRE(
            plan.targets[index].presentationIndexHint
            == plan.targets[index - 1].presentationIndexHint + 1);
        VIDSCOPE_REQUIRE(
            plan.targets[index].requestedTime - plan.targets[index - 1].requestedTime == 20ms);
    }
}

VIDSCOPE_TEST(FilmstripModel_around_current_position_falls_back_to_bounded_timestamp_requests)
{
    vidscope::timeline::TimelineModel timeline;
    timeline.reset(1s);
    VIDSCOPE_REQUIRE(timeline.setPlayhead(500ms));

    vidscope::filmstrip::FilmstripModel model;
    model.setMode(vidscope::filmstrip::FilmstripMode::AroundCurrentPosition);
    model.setCount(8);
    const auto plan = model.makePlan(timeline);

    VIDSCOPE_REQUIRE(plan.status == vidscope::filmstrip::FilmstripPlanStatus::Ready);
    VIDSCOPE_REQUIRE(!plan.usesExactContiguousFrames);
    VIDSCOPE_REQUIRE(plan.targets.size() == 8);
    VIDSCOPE_REQUIRE(plan.targets.front().requestedTime == 340ms);
    VIDSCOPE_REQUIRE(plan.targets.back().requestedTime == 620ms);
    requireStrictlyIncreasing(plan);
    for (const auto& target : plan.targets) {
        VIDSCOPE_REQUIRE(target.requestedTime >= 0ns);
        VIDSCOPE_REQUIRE(target.requestedTime <= timeline.duration());
    }
}

VIDSCOPE_TEST(FilmstripModel_reports_no_media_without_generating_work)
{
    vidscope::timeline::TimelineModel timeline;
    vidscope::filmstrip::FilmstripModel model;
    const auto plan = model.makePlan(timeline);
    VIDSCOPE_REQUIRE(plan.status == vidscope::filmstrip::FilmstripPlanStatus::NoMedia);
    VIDSCOPE_REQUIRE(plan.targets.empty());
}
