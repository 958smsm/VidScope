#include "TestHarness.h"

#include "media/MediaSource.h"
#include "thumbnails/ThumbnailManager.h"
#include "timeline/TimelineWidget.h"
#include "widgets/FilmstripController.h"
#include "widgets/FilmstripWidget.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QWidget>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

using namespace std::chrono_literals;

namespace {

using vidscope::filmstrip::FilmstripMode;
using vidscope::filmstrip::FilmstripPlan;
using vidscope::filmstrip::FilmstripPlanStatus;
using vidscope::filmstrip::FilmstripTarget;
using vidscope::media::MediaInfo;
using vidscope::media::MediaInfoPtr;
using vidscope::media::MediaSource;
using vidscope::thumbnails::ThumbnailManager;
using vidscope::thumbnails::ThumbnailManagerConfig;
using vidscope::timeline::TimelineWidget;
using vidscope::widgets::FilmstripController;
using vidscope::widgets::FilmstripControllerConfig;
using vidscope::widgets::FilmstripWidget;

std::filesystem::path fixtureDirectory;

[[nodiscard]] std::filesystem::path fixturePath(const char* name)
{
    const auto path = fixtureDirectory / name;
    VIDSCOPE_REQUIRE_MESSAGE(std::filesystem::exists(path), path.string());
    return path;
}

[[nodiscard]] MediaInfoPtr loadMediaInfo(const std::filesystem::path& path)
{
    const auto source = MediaSource::open(path);
    VIDSCOPE_REQUIRE(source != nullptr);
    return std::make_shared<MediaInfo>(source->info());
}

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate&& predicate, const int timeoutMilliseconds = 20'000)
{
    if (predicate()) {
        return true;
    }

    QEventLoop loop;
    QTimer poll;
    QTimer timeout;
    poll.setInterval(2);
    timeout.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (predicate()) {
            loop.quit();
        }
    });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start();
    timeout.start(timeoutMilliseconds);
    loop.exec();
    return predicate();
}

[[nodiscard]] ThumbnailManagerConfig managerConfig(const QString& cacheDirectory)
{
    ThumbnailManagerConfig config;
    config.workerCount = 2;
    config.maximumPendingRequests = 80;
    config.workerFrameCacheBytes = 24U * 1024U * 1024U;
    config.workerQueueBytes = 8U * 1024U * 1024U;
    config.workerQueueFrames = 3;
    config.maximumThumbnailSize = QSize(320, 180);
    config.hardwareAcceleration = vidscope::media::HardwareAcceleration::Disabled;
    config.cache.memoryBudgetBytes = 24U * 1024U * 1024U;
    config.cache.diskBudgetBytes = 64U * 1024U * 1024U;
    config.cache.diskDirectory = cacheDirectory;
    return config;
}

[[nodiscard]] FilmstripControllerConfig controllerConfig()
{
    FilmstripControllerConfig config;
    config.targetSize = QSize(120, 68);
    config.rangeRefreshDebounceMilliseconds = 0;
    config.playheadRefreshMilliseconds = 0;
    config.cancelledRetryMilliseconds = 25;
    return config;
}

[[nodiscard]] FilmstripPlan samplePlan()
{
    FilmstripPlan plan;
    plan.status = FilmstripPlanStatus::Ready;
    plan.mode = FilmstripMode::EntireVideo;
    plan.requestedCount = 3;
    plan.rangeStart = 0ns;
    plan.rangeEnd = 2s;
    plan.targets = {
        FilmstripTarget{0ns, 10, true, false},
        FilmstripTarget{1s, 11, false, true},
        FilmstripTarget{2s, 12, false, false},
    };
    return plan;
}

} // namespace

VIDSCOPE_TEST(Phase5_filmstrip_widget_is_one_custom_surface_and_activates_cells)
{
    FilmstripWidget widget;
    widget.resize(600, 132);
    widget.setPlan(samplePlan());
    widget.show();
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    VIDSCOPE_REQUIRE(widget.itemCount() == 3);
    VIDSCOPE_REQUIRE(widget.findChildren<QWidget*>(
        QString{}, Qt::FindDirectChildrenOnly).empty());

    qint64 soughtTimestamp = -1;
    qint64 inspectedTimestamp = -1;
    qint64 inspectedIndex = -1;
    QObject::connect(
        &widget,
        &FilmstripWidget::seekRequested,
        &widget,
        [&](const qint64 timestamp) { soughtTimestamp = timestamp; });
    QObject::connect(
        &widget,
        &FilmstripWidget::frameInspectorRequested,
        &widget,
        [&](const qint64 timestamp, const qint64 presentationIndex) {
            inspectedTimestamp = timestamp;
            inspectedIndex = presentationIndex;
        });

    const QPoint secondCell = widget.itemRect(1).center().toPoint();
    QTest::mouseClick(&widget, Qt::LeftButton, Qt::NoModifier, secondCell);
    VIDSCOPE_REQUIRE(soughtTimestamp == static_cast<qint64>((1s).count()));

    QTest::mouseDClick(&widget, Qt::LeftButton, Qt::NoModifier, secondCell);
    VIDSCOPE_REQUIRE(inspectedTimestamp == static_cast<qint64>((1s).count()));
    VIDSCOPE_REQUIRE(inspectedIndex == 11);
}

VIDSCOPE_TEST(Phase5_entire_video_batch_decodes_real_frames_asynchronously)
{
    QTemporaryDir cacheDirectory;
    VIDSCOPE_REQUIRE(cacheDirectory.isValid());
    const auto info = loadMediaInfo(fixturePath("cfr_bframes.mp4"));

    QWidget owner;
    TimelineWidget timeline(&owner);
    FilmstripWidget widget(&owner);
    timeline.setDuration(static_cast<qint64>(info->duration.count()));

    ThumbnailManager manager(managerConfig(cacheDirectory.path()), &owner);
    FilmstripController controller(
        &timeline,
        &manager,
        &widget,
        controllerConfig(),
        &owner);
    controller.setCount(8);
    manager.setMedia(info);
    controller.setMedia(info);

    VIDSCOPE_REQUIRE(widget.itemCount() == 8);
    VIDSCOPE_REQUIRE(waitUntil([&] {
        return controller.pendingRequestCount() == 0
            && widget.readyItemCount() + widget.failedItemCount() == widget.itemCount();
    }));
    VIDSCOPE_REQUIRE(widget.failedItemCount() == 0);
    VIDSCOPE_REQUIRE(widget.readyItemCount() == 8);

    qint64 previousTime = -1;
    for (std::size_t index = 0; index < widget.itemCount(); ++index) {
        const auto* item = widget.item(index);
        VIDSCOPE_REQUIRE(item != nullptr);
        VIDSCOPE_REQUIRE(item->frame.has_value());
        VIDSCOPE_REQUIRE(!item->frame->image.isNull());
        VIDSCOPE_REQUIRE(item->frame->image.width() <= 120);
        VIDSCOPE_REQUIRE(item->frame->image.height() <= 68);
        const qint64 currentTime = static_cast<qint64>(item->frame->presentationTime.count());
        VIDSCOPE_REQUIRE(currentTime >= previousTime);
        previousTime = currentTime;
    }
}

VIDSCOPE_TEST(Phase5_new_mode_supersedes_stale_filmstrip_batch)
{
    QTemporaryDir cacheDirectory;
    VIDSCOPE_REQUIRE(cacheDirectory.isValid());
    const auto info = loadMediaInfo(fixturePath("long_gop.mp4"));

    QWidget owner;
    TimelineWidget timeline(&owner);
    FilmstripWidget widget(&owner);
    timeline.setDuration(static_cast<qint64>(info->duration.count()));
    timeline.setPosition(static_cast<qint64>((1s).count()));

    ThumbnailManager manager(managerConfig(cacheDirectory.path()), &owner);
    FilmstripController controller(
        &timeline,
        &manager,
        &widget,
        controllerConfig(),
        &owner);
    controller.setCount(32);
    manager.setMedia(info);
    controller.setMedia(info);
    const quint64 firstBatch = controller.batchGeneration();

    controller.setMode(FilmstripMode::AroundCurrentPosition);
    controller.setCount(8);
    controller.setPlayhead(static_cast<qint64>((1s).count()));
    controller.refreshNow();
    const quint64 latestBatch = controller.batchGeneration();

    VIDSCOPE_REQUIRE(latestBatch != firstBatch);
    VIDSCOPE_REQUIRE(widget.plan().mode == FilmstripMode::AroundCurrentPosition);
    VIDSCOPE_REQUIRE(widget.itemCount() == 8);
    VIDSCOPE_REQUIRE(waitUntil([&] {
        return controller.batchGeneration() == latestBatch
            && controller.pendingRequestCount() == 0
            && widget.readyItemCount() + widget.failedItemCount() == widget.itemCount();
    }));
    VIDSCOPE_REQUIRE(widget.failedItemCount() == 0);
    VIDSCOPE_REQUIRE(widget.readyItemCount() == 8);
    VIDSCOPE_REQUIRE(widget.plan().mode == FilmstripMode::AroundCurrentPosition);
}

VIDSCOPE_TEST(Phase5_bounded_queue_eviction_retries_without_stuck_loading_cells)
{
    QTemporaryDir cacheDirectory;
    VIDSCOPE_REQUIRE(cacheDirectory.isValid());
    const auto info = loadMediaInfo(fixturePath("cfr_no_b.mp4"));

    QWidget owner;
    TimelineWidget timeline(&owner);
    FilmstripWidget widget(&owner);
    timeline.setDuration(static_cast<qint64>(info->duration.count()));

    auto config = managerConfig(cacheDirectory.path());
    config.workerCount = 1;
    config.maximumPendingRequests = 4;
    ThumbnailManager manager(std::move(config), &owner);
    FilmstripController controller(
        &timeline,
        &manager,
        &widget,
        controllerConfig(),
        &owner);

    int cancellationCount = 0;
    QObject::connect(
        &manager,
        &ThumbnailManager::previewCancelled,
        &owner,
        [&cancellationCount](const vidscope::thumbnails::ThumbnailGeneration) {
            ++cancellationCount;
        });

    controller.setCount(8);
    manager.setMedia(info);
    controller.setMedia(info);

    VIDSCOPE_REQUIRE(waitUntil([&] {
        return controller.pendingRequestCount() == 0
            && widget.readyItemCount() + widget.failedItemCount() == widget.itemCount();
    }));
    VIDSCOPE_REQUIRE(cancellationCount > 0);
    VIDSCOPE_REQUIRE(widget.failedItemCount() == 0);
    VIDSCOPE_REQUIRE(widget.readyItemCount() == 8);
}

VIDSCOPE_TEST(Phase5_selected_range_waits_for_selection_then_refreshes_only_that_range)
{
    QTemporaryDir cacheDirectory;
    VIDSCOPE_REQUIRE(cacheDirectory.isValid());
    const auto info = loadMediaInfo(fixturePath("cfr_no_b.mp4"));

    QWidget owner;
    TimelineWidget timeline(&owner);
    FilmstripWidget widget(&owner);
    timeline.setDuration(static_cast<qint64>(info->duration.count()));

    ThumbnailManager manager(managerConfig(cacheDirectory.path()), &owner);
    FilmstripController controller(
        &timeline,
        &manager,
        &widget,
        controllerConfig(),
        &owner);
    controller.setCount(8);
    controller.setMode(FilmstripMode::SelectedRange);
    manager.setMedia(info);
    controller.setMedia(info);

    VIDSCOPE_REQUIRE(
        widget.plan().status == FilmstripPlanStatus::SelectionRequired);
    VIDSCOPE_REQUIRE(widget.itemCount() == 0);

    timeline.setPosition(static_cast<qint64>((300ms).count()));
    timeline.setInPointAtPlayhead();
    timeline.setPosition(static_cast<qint64>((1'200ms).count()));
    timeline.setOutPointAtPlayhead();
    VIDSCOPE_REQUIRE(waitUntil([&] {
        return widget.plan().status == FilmstripPlanStatus::Ready
            && widget.itemCount() == 8;
    }));
    VIDSCOPE_REQUIRE(widget.plan().rangeStart == 300ms);
    VIDSCOPE_REQUIRE(widget.plan().rangeEnd == 1'200ms);
    VIDSCOPE_REQUIRE(waitUntil([&] {
        return controller.pendingRequestCount() == 0
            && widget.readyItemCount() + widget.failedItemCount() == widget.itemCount();
    }));
    VIDSCOPE_REQUIRE(widget.failedItemCount() == 0);
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        return 2;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    }

    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("VidScopeTests"));
    QApplication::setOrganizationDomain(QStringLiteral("tests.vidscope.invalid"));
    QApplication::setApplicationName(QStringLiteral("Phase5FilmstripTests"));
    application.setQuitOnLastWindowClosed(false);

    fixtureDirectory = std::filesystem::path(argv[1]);
    return vidscope::test::runAll();
}
