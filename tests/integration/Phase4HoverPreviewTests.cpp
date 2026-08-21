#include "TestHarness.h"

#include "media/MediaSource.h"
#include "thumbnails/ThumbnailManager.h"
#include "timeline/TimelineWidget.h"
#include "widgets/HoverPreviewController.h"
#include "widgets/HoverPreviewPopup.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEvent>
#include <QtCore/QEventLoop>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

using vidscope::media::MediaInfo;
using vidscope::media::MediaInfoPtr;
using vidscope::media::MediaSource;
using vidscope::thumbnails::ThumbnailCacheSource;
using vidscope::thumbnails::ThumbnailGeneration;
using vidscope::thumbnails::ThumbnailManager;
using vidscope::thumbnails::ThumbnailManagerConfig;
using vidscope::thumbnails::ThumbnailPriority;
using vidscope::thumbnails::ThumbnailResult;
using vidscope::timeline::TimelineWidget;
using vidscope::widgets::HoverPreviewConfig;
using vidscope::widgets::HoverPreviewController;
using vidscope::widgets::HoverPreviewPopup;

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
[[nodiscard]] bool waitUntil(Predicate&& predicate, int timeoutMilliseconds = 15'000)
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

[[nodiscard]] qint64 nanoseconds(std::chrono::nanoseconds value)
{
    return static_cast<qint64>(value.count());
}

[[nodiscard]] qint64 absoluteDifference(qint64 left, qint64 right) noexcept
{
    return left >= right ? left - right : right - left;
}

[[nodiscard]] ThumbnailManagerConfig managerConfig(const QString& cacheDirectory)
{
    ThumbnailManagerConfig config;
    config.workerCount = 2;
    config.maximumPendingRequests = 16;
    config.workerFrameCacheBytes = 24U * 1024U * 1024U;
    config.workerQueueBytes = 8U * 1024U * 1024U;
    config.workerQueueFrames = 3;
    config.maximumThumbnailSize = QSize(320, 180);
    config.hardwareAcceleration = vidscope::media::HardwareAcceleration::Disabled;
    config.cache.memoryBudgetBytes = 16U * 1024U * 1024U;
    config.cache.diskBudgetBytes = 64U * 1024U * 1024U;
    config.cache.diskDirectory = cacheDirectory;
    return config;
}

struct PreviewProbe final {
    std::vector<ThumbnailResult> results;
    std::vector<std::pair<ThumbnailGeneration, QString>> failures;
};

void connectProbe(ThumbnailManager& manager, PreviewProbe& probe)
{
    QObject::connect(
        &manager,
        &ThumbnailManager::previewReady,
        &manager,
        [&probe](const ThumbnailResult& result) {
            probe.results.push_back(result);
        });
    QObject::connect(
        &manager,
        &ThumbnailManager::previewFailed,
        &manager,
        [&probe](ThumbnailGeneration generation, const QString& detail) {
            probe.failures.emplace_back(generation, detail);
        });
}

} // namespace

VIDSCOPE_TEST(Phase4_latest_hover_generation_wins_and_memory_cache_is_reused)
{
    QTemporaryDir cacheDirectory;
    VIDSCOPE_REQUIRE(cacheDirectory.isValid());

    const auto info = loadMediaInfo(fixturePath("cfr_bframes.mp4"));
    PreviewProbe probe;
    ThumbnailManager manager(managerConfig(cacheDirectory.path()));
    connectProbe(manager, probe);
    manager.setMedia(info);

    const auto first = manager.requestPreview(
        nanoseconds(250ms),
        QSize(120, 68),
        ThumbnailPriority::HoverPreview);
    const auto second = manager.requestPreview(
        nanoseconds(750ms),
        QSize(120, 68),
        ThumbnailPriority::HoverPreview);
    const auto latest = manager.requestPreview(
        nanoseconds(1'250ms),
        QSize(120, 68),
        ThumbnailPriority::HoverPreview);
    VIDSCOPE_REQUIRE(first != 0);
    VIDSCOPE_REQUIRE(second != 0);
    VIDSCOPE_REQUIRE(latest != 0);

    const bool delivered = waitUntil([&] {
        return !probe.results.empty() || !probe.failures.empty();
    });
    VIDSCOPE_REQUIRE(delivered);
    VIDSCOPE_REQUIRE_MESSAGE(
        probe.failures.empty(),
        probe.failures.empty() ? std::string{} : probe.failures.back().second.toStdString());
    VIDSCOPE_REQUIRE(!probe.results.empty());
    for (const auto& result : probe.results) {
        VIDSCOPE_REQUIRE(result.request.generation == latest);
    }

    const ThumbnailResult& decoded = probe.results.back();
    VIDSCOPE_REQUIRE(decoded.cacheSource == ThumbnailCacheSource::Decoded);
    VIDSCOPE_REQUIRE(!decoded.frame.image.isNull());
    VIDSCOPE_REQUIRE(decoded.frame.image.width() <= 120);
    VIDSCOPE_REQUIRE(decoded.frame.image.height() <= 68);
    VIDSCOPE_REQUIRE(decoded.frame.presentationIndex >= 0);
    VIDSCOPE_REQUIRE(
        absoluteDifference(
            static_cast<qint64>(decoded.frame.presentationTime.count()),
            nanoseconds(1'250ms))
        <= nanoseconds(100ms));
    VIDSCOPE_REQUIRE(!decoded.frame.motionScore.has_value());
    VIDSCOPE_REQUIRE(!decoded.frame.similarityScore.has_value());

    const std::size_t previousCount = probe.results.size();
    const auto cachedGeneration = manager.requestPreview(
        nanoseconds(1'250ms),
        QSize(120, 68),
        ThumbnailPriority::HoverPreview);
    VIDSCOPE_REQUIRE(cachedGeneration != 0);
    VIDSCOPE_REQUIRE(waitUntil([&] { return probe.results.size() > previousCount; }));
    VIDSCOPE_REQUIRE(probe.results.back().request.generation == cachedGeneration);
    VIDSCOPE_REQUIRE(probe.results.back().cacheSource == ThumbnailCacheSource::Memory);
}

VIDSCOPE_TEST(Phase4_disk_cache_survives_worker_pool_recreation)
{
    QTemporaryDir cacheDirectory;
    VIDSCOPE_REQUIRE(cacheDirectory.isValid());

    const auto info = loadMediaInfo(fixturePath("cfr_no_b.mp4"));
    auto config = managerConfig(cacheDirectory.path());
    config.workerCount = 1;
    config.cache.memoryBudgetBytes = 0;

    {
        PreviewProbe firstProbe;
        ThumbnailManager firstManager(config);
        connectProbe(firstManager, firstProbe);
        firstManager.setMedia(info);
        const auto generation = firstManager.requestPreview(
            nanoseconds(900ms),
            QSize(100, 56),
            ThumbnailPriority::HoverPreview);
        VIDSCOPE_REQUIRE(generation != 0);
        VIDSCOPE_REQUIRE(waitUntil([&] {
            return !firstProbe.results.empty() || !firstProbe.failures.empty();
        }));
        VIDSCOPE_REQUIRE(firstProbe.failures.empty());
        VIDSCOPE_REQUIRE(firstProbe.results.back().cacheSource == ThumbnailCacheSource::Decoded);
        // Decoded previews are intentionally delivered before disk serialization.
        // Keep the first manager alive until persistence has completed so this
        // test validates the cache rather than racing deterministic shutdown.
        VIDSCOPE_REQUIRE(waitUntil([&] {
            return firstManager.cacheStats().diskWrites >= 1;
        }));
    }

    PreviewProbe secondProbe;
    ThumbnailManager secondManager(config);
    connectProbe(secondManager, secondProbe);
    secondManager.setMedia(info);
    const auto generation = secondManager.requestPreview(
        nanoseconds(900ms),
        QSize(100, 56),
        ThumbnailPriority::HoverPreview);
    VIDSCOPE_REQUIRE(generation != 0);
    VIDSCOPE_REQUIRE(waitUntil([&] {
        return !secondProbe.results.empty() || !secondProbe.failures.empty();
    }));
    VIDSCOPE_REQUIRE(secondProbe.failures.empty());
    VIDSCOPE_REQUIRE(secondProbe.results.back().request.generation == generation);
    VIDSCOPE_REQUIRE(secondProbe.results.back().cacheSource == ThumbnailCacheSource::Disk);
    VIDSCOPE_REQUIRE(!secondProbe.results.back().frame.image.isNull());
}

VIDSCOPE_TEST(Phase4_popup_follows_timeline_hover_and_stays_inside_application_geometry)
{
    QTemporaryDir cacheDirectory;
    VIDSCOPE_REQUIRE(cacheDirectory.isValid());
    const auto info = loadMediaInfo(fixturePath("cfr_bframes.mp4"));

    QWidget anchor;
    anchor.setObjectName(QStringLiteral("phase4Anchor"));
    anchor.resize(1'000, 420);
    auto* layout = new QVBoxLayout(&anchor);
    auto* timeline = new TimelineWidget(&anchor);
    timeline->setDuration(static_cast<qint64>(info->duration.count()));
    layout->addStretch(1);
    layout->addWidget(timeline);
    anchor.show();
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    ThumbnailManager manager(managerConfig(cacheDirectory.path()));
    manager.setMedia(info);
    HoverPreviewConfig previewConfig;
    previewConfig.targetSize = QSize(160, 90);
    previewConfig.debounceMilliseconds = 0;
    HoverPreviewController controller(
        timeline,
        &manager,
        &anchor,
        previewConfig);
    HoverPreviewPopup* popup = controller.popup();
    VIDSCOPE_REQUIRE(popup != nullptr);

    const QPoint firstHover(timeline->width() / 2, timeline->height() / 2);
    QTest::mouseMove(timeline, firstHover, 1);
    VIDSCOPE_REQUIRE(waitUntil([&] {
        return popup->isVisible() && popup->hasPreviewImage();
    }));
    const auto firstGeneration = popup->generation();
    VIDSCOPE_REQUIRE(firstGeneration != 0);
    VIDSCOPE_REQUIRE(popup->displayedTimestampNanoseconds() >= 0);

    const QRect anchorBounds(anchor.mapToGlobal(QPoint(0, 0)), anchor.size());
    VIDSCOPE_REQUIRE(anchorBounds.contains(popup->geometry()));
    QScreen* screen = QGuiApplication::screenAt(popup->geometry().center());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    VIDSCOPE_REQUIRE(screen != nullptr);
    VIDSCOPE_REQUIRE(screen->availableGeometry().contains(popup->geometry()));

    // The custom track has a 12 px horizontal inset; stay inside it while
    // exercising right-edge popup clamping.
    const QPoint secondHover(std::max(13, timeline->width() - 20), timeline->height() / 2);
    QTest::mouseMove(timeline, secondHover, 1);
    VIDSCOPE_REQUIRE(waitUntil([&] {
        return popup->generation() != 0
            && popup->generation() != firstGeneration
            && popup->hasPreviewImage();
    }));
    VIDSCOPE_REQUIRE(anchorBounds.contains(popup->geometry()));

    QEvent leaveEvent(QEvent::Leave);
    QCoreApplication::sendEvent(timeline, &leaveEvent);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    VIDSCOPE_REQUIRE(!popup->isVisible());
}

VIDSCOPE_TEST(Phase4_worker_pool_shutdown_cancels_active_decode_deterministically)
{
    QTemporaryDir cacheDirectory;
    VIDSCOPE_REQUIRE(cacheDirectory.isValid());
    const auto info = loadMediaInfo(fixturePath("long_gop.mp4"));

    auto manager = std::make_unique<ThumbnailManager>(managerConfig(cacheDirectory.path()));
    manager->setMedia(info);
    for (int request = 0; request < 12; ++request) {
        const qint64 target = nanoseconds(500ms) * request;
        (void)manager->requestPreview(
            target,
            QSize(320, 180),
            ThumbnailPriority::HoverPreview);
    }

    QElapsedTimer shutdown;
    shutdown.start();
    manager.reset();
    VIDSCOPE_REQUIRE(shutdown.elapsed() < 5'000);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
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
    QApplication::setApplicationName(QStringLiteral("Phase4HoverPreviewTests"));
    application.setQuitOnLastWindowClosed(false);

    fixtureDirectory = std::filesystem::path(argv[1]);
    return vidscope::test::runAll();
}
