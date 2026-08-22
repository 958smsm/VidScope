#include "TestHarness.h"

#include "thumbnails/ThumbnailScheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <stop_token>
#include <thread>
#include <utility>

using namespace std::chrono_literals;

namespace {

using vidscope::core::CancellationSource;
using vidscope::thumbnails::ThumbnailCacheKey;
using vidscope::thumbnails::ThumbnailGeneration;
using vidscope::thumbnails::ThumbnailJob;
using vidscope::thumbnails::ThumbnailPriority;
using vidscope::thumbnails::ThumbnailScheduler;
using vidscope::thumbnails::ThumbnailTakeStatus;

[[nodiscard]] qint64 nanoseconds(std::chrono::nanoseconds value)
{
    return static_cast<qint64>(value.count());
}

struct JobAndCancellation final {
    ThumbnailJob job;
    std::shared_ptr<CancellationSource> cancellation;
};

[[nodiscard]] JobAndCancellation makeJob(
    ThumbnailGeneration generation,
    ThumbnailPriority priority,
    qint64 timestampNanoseconds)
{
    auto cancellation = std::make_shared<CancellationSource>();
    ThumbnailJob job;
    job.request.generation = generation;
    job.request.timestamp = vidscope::media::MediaTime(timestampNanoseconds);
    job.request.priority = priority;
    job.request.targetSize = QSize(160, 90);
    job.cacheKey = ThumbnailCacheKey{
        QStringLiteral("media"),
        timestampNanoseconds,
        QSize(160, 90),
    };
    job.media.identity = QStringLiteral("media");
    job.media.epoch = 1;
    job.cancellation = cancellation;
    return {std::move(job), std::move(cancellation)};
}

} // namespace

VIDSCOPE_TEST(ThumbnailScheduler_keeps_only_the_latest_pending_hover_request)
{
    ThumbnailScheduler scheduler(8, 1);
    auto first = makeJob(1, ThumbnailPriority::HoverPreview, nanoseconds(100ms));
    auto second = makeJob(2, ThumbnailPriority::HoverPreview, nanoseconds(200ms));
    std::atomic_int cancellationNotices{0};
    first.job.cancellationNotifier = [&cancellationNotices] {
        cancellationNotices.fetch_add(1, std::memory_order_relaxed);
    };

    VIDSCOPE_REQUIRE(scheduler.schedule(std::move(first.job)));
    VIDSCOPE_REQUIRE(scheduler.schedule(std::move(second.job)));
    VIDSCOPE_REQUIRE(first.cancellation->isCancellationRequested());
    VIDSCOPE_REQUIRE(cancellationNotices.load(std::memory_order_relaxed) == 1);
    VIDSCOPE_REQUIRE(!second.cancellation->isCancellationRequested());
    VIDSCOPE_REQUIRE(scheduler.pendingCount() == 1);

    auto taken = scheduler.waitTake(0, std::stop_token{}, 0);
    VIDSCOPE_REQUIRE(taken.status == ThumbnailTakeStatus::Job);
    VIDSCOPE_REQUIRE(taken.job.has_value());
    VIDSCOPE_REQUIRE(taken.job->request.generation == 2);
    scheduler.complete(0, 2);
}

VIDSCOPE_TEST(ThumbnailScheduler_cancels_active_stale_interactive_work)
{
    ThumbnailScheduler scheduler(8, 1);
    auto first = makeJob(10, ThumbnailPriority::HoverPreview, nanoseconds(100ms));
    VIDSCOPE_REQUIRE(scheduler.schedule(std::move(first.job)));
    auto active = scheduler.waitTake(0, std::stop_token{}, 0);
    VIDSCOPE_REQUIRE(active.status == ThumbnailTakeStatus::Job);
    VIDSCOPE_REQUIRE(active.job->request.generation == 10);

    auto latest = makeJob(11, ThumbnailPriority::HoverPreview, nanoseconds(900ms));
    VIDSCOPE_REQUIRE(scheduler.schedule(std::move(latest.job)));
    VIDSCOPE_REQUIRE(first.cancellation->isCancellationRequested());
    VIDSCOPE_REQUIRE(!latest.cancellation->isCancellationRequested());

    scheduler.complete(0, 10);
    auto replacement = scheduler.waitTake(0, std::stop_token{}, 0);
    VIDSCOPE_REQUIRE(replacement.status == ThumbnailTakeStatus::Job);
    VIDSCOPE_REQUIRE(replacement.job->request.generation == 11);
    scheduler.complete(0, 11);
}

VIDSCOPE_TEST(ThumbnailScheduler_does_not_replace_higher_priority_duplicate_work)
{
    ThumbnailScheduler scheduler(8, 1);
    auto visible = makeJob(20, ThumbnailPriority::VisibleThumbnail, nanoseconds(500ms));
    auto background = makeJob(21, ThumbnailPriority::BackgroundPrecache, nanoseconds(500ms));
    std::atomic_int cancellationNotices{0};
    visible.job.cancellationNotifier = [&cancellationNotices] {
        cancellationNotices.fetch_add(1, std::memory_order_relaxed);
    };

    VIDSCOPE_REQUIRE(scheduler.schedule(std::move(visible.job)));
    VIDSCOPE_REQUIRE(!scheduler.schedule(std::move(background.job)));
    VIDSCOPE_REQUIRE(background.cancellation->isCancellationRequested());
    VIDSCOPE_REQUIRE(!visible.cancellation->isCancellationRequested());

    auto hover = makeJob(22, ThumbnailPriority::HoverPreview, nanoseconds(500ms));
    VIDSCOPE_REQUIRE(scheduler.schedule(std::move(hover.job)));
    VIDSCOPE_REQUIRE(visible.cancellation->isCancellationRequested());
    VIDSCOPE_REQUIRE(cancellationNotices.load(std::memory_order_relaxed) == 1);

    auto taken = scheduler.waitTake(0, std::stop_token{}, 0);
    VIDSCOPE_REQUIRE(taken.status == ThumbnailTakeStatus::Job);
    VIDSCOPE_REQUIRE(taken.job->request.generation == 22);
    scheduler.complete(0, 22);
}

VIDSCOPE_TEST(ThumbnailScheduler_notifies_pending_queue_eviction_once)
{
    ThumbnailScheduler scheduler(1, 1);
    auto first = makeJob(25, ThumbnailPriority::VisibleThumbnail, nanoseconds(100ms));
    auto replacement = makeJob(26, ThumbnailPriority::VisibleThumbnail, nanoseconds(200ms));
    std::atomic_int cancellationNotices{0};
    first.job.cancellationNotifier = [&cancellationNotices] {
        cancellationNotices.fetch_add(1, std::memory_order_relaxed);
    };

    VIDSCOPE_REQUIRE(scheduler.schedule(std::move(first.job)));
    VIDSCOPE_REQUIRE(scheduler.schedule(std::move(replacement.job)));
    VIDSCOPE_REQUIRE(first.cancellation->isCancellationRequested());
    VIDSCOPE_REQUIRE(cancellationNotices.load(std::memory_order_relaxed) == 1);

    auto taken = scheduler.waitTake(0, std::stop_token{}, 0);
    VIDSCOPE_REQUIRE(taken.status == ThumbnailTakeStatus::Job);
    VIDSCOPE_REQUIRE(taken.job->request.generation == 26);
    scheduler.complete(0, 26);
    scheduler.close();
    VIDSCOPE_REQUIRE(cancellationNotices.load(std::memory_order_relaxed) == 1);
}

VIDSCOPE_TEST(ThumbnailScheduler_maintenance_cancels_jobs_and_wakes_workers)
{
    ThumbnailScheduler scheduler(8, 1);
    auto pending = makeJob(30, ThumbnailPriority::NearPlayhead, nanoseconds(750ms));
    VIDSCOPE_REQUIRE(scheduler.schedule(std::move(pending.job)));

    const std::uint64_t generation = scheduler.requestMaintenance();
    VIDSCOPE_REQUIRE(generation != 0);
    VIDSCOPE_REQUIRE(pending.cancellation->isCancellationRequested());
    VIDSCOPE_REQUIRE(scheduler.pendingCount() == 0);

    const auto taken = scheduler.waitTake(0, std::stop_token{}, 0);
    VIDSCOPE_REQUIRE(taken.status == ThumbnailTakeStatus::Maintenance);
    VIDSCOPE_REQUIRE(taken.maintenanceGeneration == generation);
}

VIDSCOPE_TEST(ThumbnailScheduler_dispatches_priority_then_newest_sequence)
{
    ThumbnailScheduler scheduler(8, 1);
    auto background = makeJob(40, ThumbnailPriority::BackgroundPrecache, nanoseconds(100ms));
    auto olderVisible = makeJob(41, ThumbnailPriority::VisibleThumbnail, nanoseconds(200ms));
    auto newerVisible = makeJob(42, ThumbnailPriority::VisibleThumbnail, nanoseconds(300ms));

    VIDSCOPE_REQUIRE(scheduler.schedule(std::move(background.job)));
    VIDSCOPE_REQUIRE(scheduler.schedule(std::move(olderVisible.job)));
    VIDSCOPE_REQUIRE(scheduler.schedule(std::move(newerVisible.job)));

    auto first = scheduler.waitTake(0, std::stop_token{}, 0);
    VIDSCOPE_REQUIRE(first.status == ThumbnailTakeStatus::Job);
    VIDSCOPE_REQUIRE(first.job->request.generation == 42);
    scheduler.complete(0, 42);

    auto second = scheduler.waitTake(0, std::stop_token{}, 0);
    VIDSCOPE_REQUIRE(second.status == ThumbnailTakeStatus::Job);
    VIDSCOPE_REQUIRE(second.job->request.generation == 41);
    scheduler.complete(0, 41);

    auto third = scheduler.waitTake(0, std::stop_token{}, 0);
    VIDSCOPE_REQUIRE(third.status == ThumbnailTakeStatus::Job);
    VIDSCOPE_REQUIRE(third.job->request.generation == 40);
    scheduler.complete(0, 40);
}

VIDSCOPE_TEST(ThumbnailScheduler_stop_token_wakes_an_idle_worker)
{
    ThumbnailScheduler scheduler(8, 1);
    std::promise<void> enteredWait;
    auto entered = enteredWait.get_future();
    ThumbnailTakeStatus status = ThumbnailTakeStatus::Job;

    std::jthread worker([&](std::stop_token stop) {
        enteredWait.set_value();
        status = scheduler.waitTake(0, stop, 0).status;
    });
    entered.wait();
    worker.request_stop();
    worker.join();

    VIDSCOPE_REQUIRE(status == ThumbnailTakeStatus::Closed);
}
