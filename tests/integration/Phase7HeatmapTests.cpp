#include "TestHarness.h"

#include "analysis/AnalysisManager.h"
#include "media/MediaSource.h"
#include "timeline/TimelineHeatmapRenderer.h"
#include "timeline/TimelineWidget.h"
#include "widgets/MainWindow.h"

#include <QtCore/QByteArray>
#include <QtCore/QEventLoop>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtWidgets/QApplication>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

using namespace std::chrono_literals;

namespace {

using vidscope::analysis::AnalysisBucket;
using vidscope::analysis::AnalysisLodView;
using vidscope::analysis::AnalysisManager;
using vidscope::analysis::AnalysisManagerConfig;
using vidscope::analysis::AnalysisState;
using vidscope::media::MediaInfo;
using vidscope::media::MediaInfoPtr;
using vidscope::media::MediaSource;
using vidscope::timeline::CombinedHeatmapWeights;
using vidscope::timeline::HeatmapMode;
using vidscope::timeline::TimelineHeatmapRenderer;
using vidscope::timeline::TimelineWidget;
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

[[nodiscard]] std::size_t paintedPixelCount(const QImage& image)
{
    std::size_t count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 0) {
                ++count;
            }
        }
    }
    return count;
}

} // namespace

VIDSCOPE_TEST(Phase7_progressive_analysis_publishes_bounded_lod_views)
{
    QTemporaryDir cacheDirectory;
    VIDSCOPE_REQUIRE(cacheDirectory.isValid());
    const auto info = loadMediaInfo("cfr_no_b.mp4");
    AnalysisManager manager(configuration(cacheDirectory.path()));
    manager.setMedia(info);

    VIDSCOPE_REQUIRE(waitUntil([&] { return manager.state() == AnalysisState::Complete; }));
    const auto view = manager.lodView(
        0,
        static_cast<qint64>(info->duration.count()),
        8);
    VIDSCOPE_REQUIRE(!view.buckets.empty());
    VIDSCOPE_REQUIRE(view.buckets.size() <= 8);
    VIDSCOPE_REQUIRE(view.level > 0);

    std::uint64_t aggregatedSamples = 0;
    for (const auto& bucket : view.buckets) {
        aggregatedSamples += bucket.sampleCount;
        VIDSCOPE_REQUIRE(bucket.start <= bucket.end);
        VIDSCOPE_REQUIRE(bucket.motionCount <= bucket.sampleCount);
        VIDSCOPE_REQUIRE(bucket.similarityCount <= bucket.sampleCount);
        VIDSCOPE_REQUIRE(bucket.sceneCount <= bucket.sampleCount);
    }
    VIDSCOPE_REQUIRE(aggregatedSamples == static_cast<std::uint64_t>(manager.sampleCount()));

    TimelineWidget timeline;
    timeline.resize(640, 140);
    timeline.setDuration(static_cast<qint64>(info->duration.count()));
    QImage withoutHeatmap(timeline.size(), QImage::Format_ARGB32_Premultiplied);
    withoutHeatmap.fill(Qt::transparent);
    timeline.render(&withoutHeatmap);
    timeline.setAnalysisManager(&manager);
    QImage withHeatmap(timeline.size(), QImage::Format_ARGB32_Premultiplied);
    withHeatmap.fill(Qt::transparent);
    timeline.render(&withHeatmap);
    VIDSCOPE_REQUIRE(withHeatmap != withoutHeatmap);

    QImage cachedRepeat(timeline.size(), QImage::Format_ARGB32_Premultiplied);
    cachedRepeat.fill(Qt::transparent);
    timeline.render(&cachedRepeat);
    VIDSCOPE_REQUIRE(cachedRepeat == withHeatmap);

    timeline.setHeatmapMode(HeatmapMode::Motion);
    QImage invalidated(timeline.size(), QImage::Format_ARGB32_Premultiplied);
    invalidated.fill(Qt::transparent);
    timeline.render(&invalidated);
    VIDSCOPE_REQUIRE(invalidated != cachedRepeat);
}

VIDSCOPE_TEST(Phase7_renderer_supports_motion_similarity_and_configurable_combined_modes)
{
    AnalysisBucket bucket;
    bucket.start = 0s;
    bucket.end = 1s;
    bucket.sampleCount = 4;
    bucket.motionCount = 4;
    bucket.similarityCount = 4;
    bucket.sceneCount = 4;
    bucket.averageMotion = 0.8F;
    bucket.maxMotion = 0.9F;
    bucket.averageSimilarity = 0.25F;
    bucket.minSimilarity = 0.1F;
    bucket.maxSimilarity = 0.4F;
    bucket.averageSceneScore = 0.65F;
    bucket.minSceneScore = 0.5F;
    bucket.maxSceneScore = 0.9F;

    const auto motion = TimelineHeatmapRenderer::averageScore(bucket, HeatmapMode::Motion);
    const auto similarity = TimelineHeatmapRenderer::averageScore(bucket, HeatmapMode::Similarity);
    const auto scene = TimelineHeatmapRenderer::averageScore(bucket, HeatmapMode::SceneChange);
    VIDSCOPE_REQUIRE(motion.has_value());
    VIDSCOPE_REQUIRE(similarity.has_value());
    VIDSCOPE_REQUIRE(scene.has_value());
    VIDSCOPE_REQUIRE(std::abs(*motion - 0.8F) < 0.0001F);
    VIDSCOPE_REQUIRE(std::abs(*similarity - 0.25F) < 0.0001F);
    VIDSCOPE_REQUIRE(std::abs(*scene - 0.65F) < 0.0001F);

    CombinedHeatmapWeights motionOnly;
    motionOnly.motion = 1.0F;
    motionOnly.similarityDifference = 0.0F;
    motionOnly.sceneChange = 0.0F;
    const auto combined = TimelineHeatmapRenderer::averageScore(
        bucket,
        HeatmapMode::Combined,
        motionOnly);
    VIDSCOPE_REQUIRE(combined.has_value());
    VIDSCOPE_REQUIRE(std::abs(*combined - 0.8F) < 0.0001F);

    AnalysisLodView view;
    view.rangeStart = 0s;
    view.rangeEnd = 1s;
    view.bucketDuration = 1s;
    view.buckets.push_back(bucket);
    TimelineHeatmapRenderer renderer;
    for (const auto mode : {
             HeatmapMode::Motion,
             HeatmapMode::Similarity,
             HeatmapMode::SceneChange,
             HeatmapMode::Combined}) {
        QImage image(120, 40, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        renderer.paint(painter, QRectF(0.0, 0.0, 120.0, 40.0), view, mode);
        painter.end();
        VIDSCOPE_REQUIRE(paintedPixelCount(image) > 0);
    }
}

VIDSCOPE_TEST(Phase7_main_window_exposes_exclusive_heatmap_mode_actions)
{
    QSettings settings;
    settings.clear();
    MainWindow window;
    auto* timeline = window.findChild<TimelineWidget*>(QStringLiteral("timelineWidget"));
    auto* motion = window.findChild<QAction*>(QStringLiteral("actionHeatmapMotion"));
    auto* similarity = window.findChild<QAction*>(QStringLiteral("actionHeatmapSimilarity"));
    auto* scene = window.findChild<QAction*>(QStringLiteral("actionHeatmapSceneChange"));
    auto* combined = window.findChild<QAction*>(QStringLiteral("actionHeatmapCombined"));
    VIDSCOPE_REQUIRE(timeline != nullptr);
    VIDSCOPE_REQUIRE(motion != nullptr);
    VIDSCOPE_REQUIRE(similarity != nullptr);
    VIDSCOPE_REQUIRE(scene != nullptr);
    VIDSCOPE_REQUIRE(combined != nullptr);
    VIDSCOPE_REQUIRE(combined->isChecked());
    VIDSCOPE_REQUIRE(timeline->heatmapMode() == HeatmapMode::Combined);

    motion->trigger();
    VIDSCOPE_REQUIRE(motion->isChecked());
    VIDSCOPE_REQUIRE(!combined->isChecked());
    VIDSCOPE_REQUIRE(timeline->heatmapMode() == HeatmapMode::Motion);

    similarity->trigger();
    VIDSCOPE_REQUIRE(similarity->isChecked());
    VIDSCOPE_REQUIRE(!motion->isChecked());
    VIDSCOPE_REQUIRE(timeline->heatmapMode() == HeatmapMode::Similarity);

    scene->trigger();
    VIDSCOPE_REQUIRE(scene->isChecked());
    VIDSCOPE_REQUIRE(!similarity->isChecked());
    VIDSCOPE_REQUIRE(timeline->heatmapMode() == HeatmapMode::SceneChange);
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
    QCoreApplication::setApplicationName(QStringLiteral("Phase7HeatmapTests"));
    fixtureDirectory = std::filesystem::path(argv[1]);
    return vidscope::test::runAll();
}
