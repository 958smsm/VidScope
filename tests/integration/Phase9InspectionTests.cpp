#include "TestHarness.h"

#include "analysis/AnalysisTypes.h"
#include "inspection/FrameComparison.h"
#include "playback/PlaybackController.h"
#include "render/VideoViewport.h"
#include "widgets/FrameInspectorPanel.h"
#include "widgets/MainWindow.h"

#include <QtCore/QByteArray>
#include <QtCore/QEventLoop>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QMouseEvent>
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTreeWidget>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>

using namespace std::chrono_literals;

namespace {

using vidscope::analysis::AnalysisSample;
using vidscope::inspection::ComparisonMode;
using vidscope::inspection::FrameComparisonManager;
using vidscope::media::DecodedFrame;
using vidscope::media::DecodedFramePtr;
using vidscope::playback::PlaybackController;
using vidscope::render::VideoViewport;
using vidscope::widgets::FrameInspectorPanel;
using vidscope::widgets::MainWindow;

std::filesystem::path fixtureDirectory;

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

[[nodiscard]] DecodedFramePtr inspectionFrame(
    const std::int64_t index,
    const vidscope::media::MediaTime time)
{
    auto frame = std::make_shared<DecodedFrame>();
    frame->id.presentationIndex = index;
    frame->id.pts = index * 1'001;
    frame->id.sessionSerial = static_cast<std::uint64_t>(index + 1);
    frame->dts = index * 997;
    frame->presentationTime = time;
    frame->duration = 40ms;
    frame->timeBase = AVRational{1, 25'000};
    frame->keyFrame = index == 42;
    frame->pictureType = index == 42 ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_P;
    frame->width = 32;
    frame->height = 24;
    frame->pixelFormat = AV_PIX_FMT_BGRA;
    frame->bitDepth = 8;
    frame->colorRange = AVCOL_RANGE_MPEG;
    frame->colorSpace = AVCOL_SPC_BT709;
    frame->colorPrimaries = AVCOL_PRI_BT709;
    frame->colorTransfer = AVCOL_TRC_BT709;
    return frame;
}

[[nodiscard]] QString metadataValue(QTreeWidget* tree, const QString& property)
{
    if (tree == nullptr) {
        return {};
    }
    for (int index = 0; index < tree->topLevelItemCount(); ++index) {
        auto* item = tree->topLevelItem(index);
        if (item != nullptr && item->text(0) == property) {
            return item->text(1);
        }
    }
    return {};
}

[[nodiscard]] QImage solidImage(const QColor color)
{
    QImage image(QSize(32, 24), QImage::Format_RGBA8888);
    image.fill(color);
    return image;
}

} // namespace

VIDSCOPE_TEST(Phase9_comparison_manager_delivers_only_the_latest_generation)
{
    FrameComparisonManager manager;
    const QImage black = solidImage(Qt::black).scaled(768, 432);
    const QImage white = solidImage(Qt::white).scaled(768, 432);
    quint64 deliveredGeneration = 0;
    int deliveries = 0;
    QObject::connect(
        &manager,
        &FrameComparisonManager::comparisonReady,
        [&](const auto& result) {
            deliveredGeneration = result.generation;
            ++deliveries;
        });

    (void)manager.request(black, white, ComparisonMode::SsimMap);
    const quint64 latest = manager.request(black, black, ComparisonMode::SideBySide);
    VIDSCOPE_REQUIRE(waitUntil([&] { return deliveredGeneration == latest; }));
    QApplication::processEvents();
    VIDSCOPE_REQUIRE(deliveries == 1);
}

VIDSCOPE_TEST(Phase9_panel_exposes_metadata_pixel_magnifier_navigation_and_ab_metrics)
{
    FrameInspectorPanel panel;
    VideoViewport viewport;
    panel.resize(460, 900);
    viewport.resize(480, 360);
    panel.show();
    viewport.show();
    QObject::connect(
        &panel,
        &FrameInspectorPanel::pixelInspectionChanged,
        &viewport,
        &VideoViewport::setPixelInspectionEnabled);
    QObject::connect(
        &viewport,
        &VideoViewport::pixelInspected,
        &panel,
        &FrameInspectorPanel::updatePixel);
    QObject::connect(
        &viewport,
        &VideoViewport::pixelInspectionLeft,
        &panel,
        &FrameInspectorPanel::clearPixel);
    QObject::connect(
        &panel,
        &FrameInspectorPanel::comparisonDisplayChanged,
        [&](const QImage& a,
            const QImage& b,
            const ComparisonMode mode,
            const QImage& visualization,
            const QString& detail) {
            viewport.setComparison(a, b, mode, visualization, detail);
        });
    QObject::connect(
        &panel,
        &FrameInspectorPanel::comparisonCleared,
        &viewport,
        &VideoViewport::clearComparison);

    AnalysisSample analysis;
    analysis.motion = 0.125F;
    analysis.similarity = 0.875F;
    analysis.sceneScore = 0.625F;
    const QImage black = solidImage(Qt::black);
    panel.setFrame(inspectionFrame(42, 1680ms), black, analysis);
    panel.setPaused(true);
    viewport.setFrame(black);
    QApplication::processEvents();

    auto* metadata =
        panel.findChild<QTreeWidget*>(QStringLiteral("frameInspectorMetadata"));
    VIDSCOPE_REQUIRE(metadata != nullptr);
    VIDSCOPE_REQUIRE(metadata->topLevelItemCount() == 18);
    VIDSCOPE_REQUIRE(metadataValue(metadata, QStringLiteral("Frame index")) == QStringLiteral("42"));
    VIDSCOPE_REQUIRE(metadataValue(metadata, QStringLiteral("PTS")) == QStringLiteral("42042"));
    VIDSCOPE_REQUIRE(metadataValue(metadata, QStringLiteral("Matrix")).contains(
        QStringLiteral("bt709"), Qt::CaseInsensitive));
    VIDSCOPE_REQUIRE(metadataValue(metadata, QStringLiteral("Scene score")).contains(
        QStringLiteral("0.6250")));

    auto* pixelEnabled =
        panel.findChild<QCheckBox*>(QStringLiteral("pixelInspectorEnabled"));
    auto* magnification =
        panel.findChild<QComboBox*>(QStringLiteral("pixelMagnification"));
    auto* pixelReadout = panel.findChild<QLabel*>(QStringLiteral("pixelReadout"));
    VIDSCOPE_REQUIRE(pixelEnabled != nullptr);
    VIDSCOPE_REQUIRE(magnification != nullptr);
    VIDSCOPE_REQUIRE(pixelReadout != nullptr);
    VIDSCOPE_REQUIRE(pixelEnabled->isEnabled());
    VIDSCOPE_REQUIRE(magnification->count() == 4);
    QSignalSpy pixelSpy(&viewport, &VideoViewport::pixelInspected);
    pixelEnabled->setChecked(true);
    const QPoint localPosition(viewport.width() / 2, viewport.height() / 2);
    QMouseEvent moveEvent(
        QEvent::MouseMove,
        QPointF(localPosition),
        QPointF(viewport.mapToGlobal(localPosition)),
        Qt::NoButton,
        Qt::NoButton,
        Qt::NoModifier);
    QApplication::sendEvent(&viewport, &moveEvent);
    VIDSCOPE_REQUIRE(pixelSpy.count() >= 1);
    const QList<QVariant> pixelArguments = pixelSpy.takeLast();
    const int inspectedX = pixelArguments.at(0).toInt();
    const int inspectedY = pixelArguments.at(1).toInt();
    VIDSCOPE_REQUIRE(inspectedX >= 0 && inspectedX < black.width());
    VIDSCOPE_REQUIRE(inspectedY >= 0 && inspectedY < black.height());
    VIDSCOPE_REQUIRE(pixelReadout->text().contains(
        QStringLiteral("X: %1").arg(inspectedX)));
    VIDSCOPE_REQUIRE(pixelReadout->text().contains(
        QStringLiteral("Y: %1").arg(inspectedY)));
    VIDSCOPE_REQUIRE(pixelReadout->text().contains(QStringLiteral("RGB: 0, 0, 0")));

    QSignalSpy previousSpy(&panel, &FrameInspectorPanel::previousFrameRequested);
    QSignalSpy nextSpy(&panel, &FrameInspectorPanel::nextFrameRequested);
    QTest::mouseClick(
        panel.findChild<QPushButton*>(QStringLiteral("inspectorPreviousFrame")),
        Qt::LeftButton);
    QTest::mouseClick(
        panel.findChild<QPushButton*>(QStringLiteral("inspectorNextFrame")),
        Qt::LeftButton);
    VIDSCOPE_REQUIRE(previousSpy.count() == 1);
    VIDSCOPE_REQUIRE(nextSpy.count() == 1);

    panel.setCurrentAsFrameA();
    panel.setFrame(inspectionFrame(43, 1720ms), solidImage(Qt::white));
    panel.setPaused(true);
    panel.setCurrentAsFrameB();
    VIDSCOPE_REQUIRE(panel.hasFrameA());
    VIDSCOPE_REQUIRE(panel.hasFrameB());
    VIDSCOPE_REQUIRE(waitUntil([&] { return panel.comparisonMetrics().comparable; }));
    VIDSCOPE_REQUIRE(panel.comparisonMetrics().ssim < 0.001);
    VIDSCOPE_REQUIRE(std::abs(panel.comparisonMetrics().psnrDb) < 0.0001);
    VIDSCOPE_REQUIRE(viewport.comparisonActive());

    auto* mode = panel.findChild<QComboBox*>(QStringLiteral("comparisonMode"));
    VIDSCOPE_REQUIRE(mode != nullptr);
    const int differenceIndex = mode->findData(
        static_cast<int>(ComparisonMode::AbsoluteDifference));
    VIDSCOPE_REQUIRE(differenceIndex >= 0);
    mode->setCurrentIndex(differenceIndex);
    VIDSCOPE_REQUIRE(waitUntil([&] {
        return viewport.comparisonMode() == ComparisonMode::AbsoluteDifference
            && panel.comparisonMetrics().comparable;
    }));
    panel.clearComparison();
    VIDSCOPE_REQUIRE(!viewport.comparisonActive());
}

VIDSCOPE_TEST(Phase9_main_window_integrates_real_frames_actions_zoom_and_comparison)
{
    QSettings settings;
    settings.clear();
    MainWindow window;
    window.show();
    auto* panel =
        window.findChild<FrameInspectorPanel*>(QStringLiteral("frameInspectorPanel"));
    auto* dock = window.findChild<QDockWidget*>(QStringLiteral("frameInspectorDock"));
    auto* viewport = window.findChild<VideoViewport*>(QStringLiteral("videoViewport"));
    auto* controller =
        window.findChild<PlaybackController*>(QStringLiteral("playbackController"));
    auto* setA = window.findChild<QAction*>(QStringLiteral("actionSetFrameA"));
    auto* setB = window.findChild<QAction*>(QStringLiteral("actionSetFrameB"));
    auto* clear = window.findChild<QAction*>(QStringLiteral("actionClearFrameComparison"));
    VIDSCOPE_REQUIRE(panel != nullptr);
    VIDSCOPE_REQUIRE(dock != nullptr);
    VIDSCOPE_REQUIRE(viewport != nullptr);
    VIDSCOPE_REQUIRE(controller != nullptr);
    VIDSCOPE_REQUIRE(setA != nullptr);
    VIDSCOPE_REQUIRE(setB != nullptr);
    VIDSCOPE_REQUIRE(clear != nullptr);

    const auto mediaPath = fixtureDirectory / "cfr_no_b.mp4";
    controller->openFile(QString::fromStdWString(mediaPath.wstring()));
    VIDSCOPE_REQUIRE(waitUntil([&] { return panel->hasCurrentFrame(); }));
    QApplication::processEvents();
    VIDSCOPE_REQUIRE(dock->isVisible());
    VIDSCOPE_REQUIRE(setA->isEnabled());
    VIDSCOPE_REQUIRE(setB->isEnabled());

    auto* metadata =
        panel->findChild<QTreeWidget*>(QStringLiteral("frameInspectorMetadata"));
    const QString firstIndex = metadataValue(metadata, QStringLiteral("Frame index"));
    setA->trigger();
    VIDSCOPE_REQUIRE(panel->hasFrameA());
    controller->nextFrame();
    VIDSCOPE_REQUIRE(waitUntil([&] {
        return metadataValue(metadata, QStringLiteral("Frame index")) != firstIndex;
    }));
    setB->trigger();
    VIDSCOPE_REQUIRE(panel->hasFrameB());
    VIDSCOPE_REQUIRE(waitUntil([&] { return panel->comparisonMetrics().comparable; }));
    VIDSCOPE_REQUIRE(viewport->comparisonActive());

    clear->trigger();
    VIDSCOPE_REQUIRE(!viewport->comparisonActive());
    auto* imageZoom =
        panel->findChild<QComboBox*>(QStringLiteral("inspectorImageZoom"));
    VIDSCOPE_REQUIRE(imageZoom != nullptr);
    imageZoom->setCurrentIndex(imageZoom->findData(2.0));
    VIDSCOPE_REQUIRE(viewport->imageZoom() == 2.0);
    auto* pixelEnabled =
        panel->findChild<QCheckBox*>(QStringLiteral("pixelInspectorEnabled"));
    VIDSCOPE_REQUIRE(pixelEnabled != nullptr);
    VIDSCOPE_REQUIRE(pixelEnabled->isEnabled());
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
    QCoreApplication::setApplicationName(QStringLiteral("Phase9InspectionTests"));
    fixtureDirectory = std::filesystem::path(argv[1]);
    return vidscope::test::runAll();
}
