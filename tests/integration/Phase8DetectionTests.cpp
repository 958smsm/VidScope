#include "TestHarness.h"

#include "analysis/AnalysisManager.h"
#include "media/MediaSource.h"
#include "playback/PlaybackController.h"
#include "timeline/TimelineWidget.h"
#include "widgets/AnalysisResultsPanel.h"
#include "widgets/MainWindow.h"

#include <QtCore/QByteArray>
#include <QtCore/QEventLoop>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTreeWidget>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

using namespace std::chrono_literals;

namespace {

using vidscope::analysis::AnalysisManager;
using vidscope::analysis::AnalysisManagerConfig;
using vidscope::analysis::AnalysisState;
using vidscope::analysis::DetectionConfig;
using vidscope::analysis::DetectionKind;
using vidscope::analysis::DetectionResult;
using vidscope::analysis::DetectionResults;
using vidscope::media::MediaInfo;
using vidscope::media::MediaInfoPtr;
using vidscope::media::MediaSource;
using vidscope::playback::PlaybackController;
using vidscope::timeline::HeatmapMode;
using vidscope::timeline::TimelineMarkerKind;
using vidscope::timeline::TimelineWidget;
using vidscope::widgets::AnalysisResultsPanel;
using vidscope::widgets::MainWindow;

std::filesystem::path fixtureDirectory;

[[nodiscard]] MediaInfoPtr loadMediaInfo(const char* name)
{
    const auto path = fixtureDirectory / name;
    VIDSCOPE_REQUIRE_MESSAGE(std::filesystem::exists(path), path.string());
    auto source = MediaSource::open(path);
    VIDSCOPE_REQUIRE(source != nullptr);
    return std::make_shared<MediaInfo>(source->info());
}

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate&& predicate, const int timeoutMilliseconds = 30'000)
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

[[nodiscard]] AnalysisManagerConfig configuration(const QString& cacheDirectory)
{
    AnalysisManagerConfig config;
    config.cache.diskDirectory = cacheDirectory;
    config.cache.diskBudgetBytes = 16U * 1024U * 1024U;
    config.cache.maximumDocumentBytes = 8U * 1024U * 1024U;
    config.maximumInMemorySamples = 1'024;
    config.cache.maximumSamples = 1'024;
    config.deliveryBatchFrames = 4;
    config.pyramid.maximumBaseBuckets = 64;
    config.session.frameCacheBytes = 4U * 1024U * 1024U;
    config.session.forwardQueueBytes = 2U * 1024U * 1024U;
    config.session.forwardQueueFrames = 2;
    config.session.initialPrefetchFrames = 1;
    return config;
}

[[nodiscard]] DetectionResult detection(
    const DetectionKind kind,
    const vidscope::media::MediaTime start,
    const vidscope::media::MediaTime end,
    const std::int64_t firstFrame,
    const std::int64_t lastFrame,
    const float score)
{
    DetectionResult result;
    result.kind = kind;
    result.start = start;
    result.end = end;
    result.firstFrame = firstFrame;
    result.lastFrame = lastFrame;
    result.frameCount = static_cast<std::size_t>(lastFrame - firstFrame + 1);
    result.score = score;
    return result;
}

} // namespace

VIDSCOPE_TEST(Phase8_analysis_produces_fingerprints_scene_scores_and_bounded_results)
{
    QTemporaryDir cacheDirectory;
    VIDSCOPE_REQUIRE(cacheDirectory.isValid());
    const auto info = loadMediaInfo("cfr_no_b.mp4");
    AnalysisManager manager(configuration(cacheDirectory.path()));
    QSignalSpy detections(&manager, &AnalysisManager::detectionsChanged);
    manager.setMedia(info);

    VIDSCOPE_REQUIRE(waitUntil([&] { return manager.state() == AnalysisState::Complete; }));
    const auto samples = manager.samplesInRange(
        0,
        static_cast<qint64>(info->duration.count()),
        1'024);
    VIDSCOPE_REQUIRE(samples.size() > 1);
    VIDSCOPE_REQUIRE(samples.front().contentHash.has_value());
    VIDSCOPE_REQUIRE(samples.front().perceptualHash.has_value());
    for (std::size_t index = 1; index < samples.size(); ++index) {
        VIDSCOPE_REQUIRE(samples[index].contentHash.has_value());
        VIDSCOPE_REQUIRE(samples[index].perceptualHash.has_value());
        VIDSCOPE_REQUIRE(samples[index].sceneScore.has_value());
        VIDSCOPE_REQUIRE(samples[index].duplicateScore.has_value());
        VIDSCOPE_REQUIRE(*samples[index].sceneScore >= 0.0F);
        VIDSCOPE_REQUIRE(*samples[index].sceneScore <= 1.0F);
        VIDSCOPE_REQUIRE(*samples[index].duplicateScore >= 0.0F);
        VIDSCOPE_REQUIRE(*samples[index].duplicateScore <= 1.0F);
    }

    const auto results = manager.detectionResults();
    VIDSCOPE_REQUIRE(results.analyzedSamples == samples.size());
    VIDSCOPE_REQUIRE(!results.scenes.empty());
    VIDSCOPE_REQUIRE(results.scenes.size() <= manager.detectionConfig().maximumResultsPerKind);
    const auto lod = manager.lodView(
        0,
        static_cast<qint64>(info->duration.count()),
        8);
    VIDSCOPE_REQUIRE(!lod.buckets.empty());
    VIDSCOPE_REQUIRE(std::any_of(lod.buckets.cbegin(), lod.buckets.cend(), [](const auto& bucket) {
        return bucket.sceneCount > 0;
    }));

    const int previousSignals = detections.count();
    auto config = manager.detectionConfig();
    config.sceneThreshold = 0.0F;
    manager.setDetectionConfig(config);
    VIDSCOPE_REQUIRE(waitUntil([&] { return detections.count() > previousSignals; }));
    VIDSCOPE_REQUIRE(manager.detectionConfig().sceneThreshold == 0.0F);
}

VIDSCOPE_TEST(Phase8_results_panel_lists_seeks_and_reanalyzes)
{
    qRegisterMetaType<DetectionConfig>();
    AnalysisResultsPanel panel;
    panel.resize(720, 520);
    panel.show();

    DetectionResults results;
    results.analyzedSamples = 120;
    results.scenes.push_back(detection(DetectionKind::SceneChange, 1s, 1s + 40ms, 25, 25, 0.91F));
    results.scenes.push_back(detection(DetectionKind::SceneChange, 4s, 4s + 40ms, 100, 100, 0.82F));
    results.duplicates.push_back(detection(DetectionKind::ExactDuplicate, 2s, 2300ms, 50, 57, 1.0F));
    auto repeated = detection(DetectionKind::RepeatedSection, 5s, 5400ms, 125, 134, 0.96F);
    repeated.matchingStart = 500ms;
    repeated.matchingEnd = 900ms;
    repeated.matchingFirstFrame = 12;
    repeated.matchingLastFrame = 21;
    results.duplicates.push_back(repeated);
    results.freezes.push_back(detection(DetectionKind::Freeze, 6s, 7s, 150, 175, 0.999F));
    panel.setResults(results);
    QApplication::processEvents();

    auto* scenes = panel.findChild<QTreeWidget*>(QStringLiteral("sceneResults"));
    auto* duplicates = panel.findChild<QTreeWidget*>(QStringLiteral("duplicateResults"));
    auto* freezes = panel.findChild<QTreeWidget*>(QStringLiteral("freezeResults"));
    VIDSCOPE_REQUIRE(scenes != nullptr);
    VIDSCOPE_REQUIRE(duplicates != nullptr);
    VIDSCOPE_REQUIRE(freezes != nullptr);
    VIDSCOPE_REQUIRE(scenes->topLevelItemCount() == 2);
    VIDSCOPE_REQUIRE(duplicates->topLevelItemCount() == 2);
    VIDSCOPE_REQUIRE(freezes->topLevelItemCount() == 1);

    QSignalSpy seekSpy(&panel, &AnalysisResultsPanel::seekRequested);
    const QRect itemRect = scenes->visualItemRect(scenes->topLevelItem(0));
    VIDSCOPE_REQUIRE(itemRect.isValid());
    QTest::mouseClick(scenes->viewport(), Qt::LeftButton, Qt::NoModifier, itemRect.center());
    VIDSCOPE_REQUIRE(seekSpy.count() == 1);
    VIDSCOPE_REQUIRE(seekSpy.at(0).at(0).toLongLong() == 1'000'000'000LL);

    auto* threshold = panel.findChild<QDoubleSpinBox*>(QStringLiteral("sceneThreshold"));
    auto* reanalyze = panel.findChild<QPushButton*>(QStringLiteral("reanalyzeDetections"));
    VIDSCOPE_REQUIRE(threshold != nullptr);
    VIDSCOPE_REQUIRE(reanalyze != nullptr);
    QSignalSpy reanalyzeSpy(&panel, &AnalysisResultsPanel::reanalyzeRequested);
    threshold->setValue(0.625);
    QTest::mouseClick(reanalyze, Qt::LeftButton);
    VIDSCOPE_REQUIRE(reanalyzeSpy.count() == 1);
    const auto submitted = qvariant_cast<DetectionConfig>(reanalyzeSpy.at(0).at(0));
    VIDSCOPE_REQUIRE(std::abs(submitted.sceneThreshold - 0.625F) < 0.0001F);
}

VIDSCOPE_TEST(Phase8_main_window_publishes_scene_heatmap_markers_and_results)
{
    QSettings settings;
    settings.clear();
    MainWindow window;
    window.show();
    auto* manager = window.findChild<AnalysisManager*>(QStringLiteral("analysisManager"));
    auto* controller = window.findChild<PlaybackController*>(QStringLiteral("playbackController"));
    auto* timeline = window.findChild<TimelineWidget*>(QStringLiteral("timelineWidget"));
    auto* panel = window.findChild<AnalysisResultsPanel*>(QStringLiteral("analysisResultsPanel"));
    auto* sceneMode = window.findChild<QAction*>(QStringLiteral("actionHeatmapSceneChange"));
    auto* nextScene = window.findChild<QAction*>(QStringLiteral("actionNextScene"));
    auto* previousScene = window.findChild<QAction*>(QStringLiteral("actionPreviousScene"));
    VIDSCOPE_REQUIRE(manager != nullptr);
    VIDSCOPE_REQUIRE(controller != nullptr);
    VIDSCOPE_REQUIRE(timeline != nullptr);
    VIDSCOPE_REQUIRE(panel != nullptr);
    VIDSCOPE_REQUIRE(sceneMode != nullptr);
    VIDSCOPE_REQUIRE(nextScene != nullptr);
    VIDSCOPE_REQUIRE(previousScene != nullptr);

    sceneMode->trigger();
    VIDSCOPE_REQUIRE(sceneMode->isChecked());
    VIDSCOPE_REQUIRE(timeline->heatmapMode() == HeatmapMode::SceneChange);

    const auto mediaPath = fixtureDirectory / "cfr_no_b.mp4";
    controller->openFile(QString::fromStdWString(mediaPath.wstring()));
    VIDSCOPE_REQUIRE(waitUntil([&] { return manager->state() == AnalysisState::Complete; }));
    QApplication::processEvents();

    const auto results = manager->detectionResults();
    VIDSCOPE_REQUIRE(!results.scenes.empty());
    const auto sceneMarkers = std::count_if(
        timeline->model().markers().begin(),
        timeline->model().markers().end(),
        [](const auto& marker) { return marker.kind == TimelineMarkerKind::Scene; });
    VIDSCOPE_REQUIRE(sceneMarkers == static_cast<std::ptrdiff_t>(results.scenes.size()));
    auto* sceneTree = panel->findChild<QTreeWidget*>(QStringLiteral("sceneResults"));
    VIDSCOPE_REQUIRE(sceneTree != nullptr);
    VIDSCOPE_REQUIRE(sceneTree->topLevelItemCount() == static_cast<int>(results.scenes.size()));
    VIDSCOPE_REQUIRE(nextScene->isEnabled());
    VIDSCOPE_REQUIRE(previousScene->isEnabled());
}

VIDSCOPE_TEST(Phase8_about_displays_the_application_version)
{
    const QString previousVersion = QApplication::applicationVersion();
    QApplication::setApplicationVersion(QStringLiteral("0.8.0-test"));
    MainWindow window;
    auto* about = window.findChild<QAction*>(QStringLiteral("actionAbout"));
    VIDSCOPE_REQUIRE(about != nullptr);

    bool dialogFound = false;
    bool versionFound = false;
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        dialogFound = dialog != nullptr;
        if (dialog != nullptr) {
            versionFound = dialog->text().contains(QStringLiteral("0.8.0-test"));
            dialog->accept();
        }
    });
    about->trigger();
    QApplication::setApplicationVersion(previousVersion);
    VIDSCOPE_REQUIRE(dialogFound);
    VIDSCOPE_REQUIRE(versionFound);
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
    QCoreApplication::setOrganizationName(QStringLiteral("VidScopeTests"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("tests.vidscope.invalid"));
    QCoreApplication::setApplicationName(QStringLiteral("Phase8DetectionTests"));
    fixtureDirectory = std::filesystem::path(argv[1]);
    return vidscope::test::runAll();
}
