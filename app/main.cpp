#include "core/Logging.h"
#include "media/FfmpegRaii.h"
#include "playback/PlaybackController.h"
#include "thumbnails/ThumbnailManager.h"
#include "timeline/TimelineWidget.h"
#include "widgets/HoverPreviewPopup.h"
#include "widgets/MainWindow.h"

#include <QtCore/QCommandLineOption>
#include <QtCore/QCommandLineParser>
#include <QtCore/QDebug>
#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtGui/QColor>
#include <QtGui/QPalette>
#include <QtWidgets/QApplication>
#include <QtWidgets/QStyleFactory>

#include <chrono>
#include <cstdlib>
#include <exception>

namespace {

using namespace std::chrono_literals;

void installApplicationPalette(QApplication& application)
{
    if (auto* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        application.setStyle(fusion);
    }

    QPalette palette;
    palette.setColor(QPalette::Window, QColor(17, 20, 25));
    palette.setColor(QPalette::WindowText, QColor(217, 222, 231));
    palette.setColor(QPalette::Base, QColor(20, 24, 30));
    palette.setColor(QPalette::AlternateBase, QColor(28, 33, 41));
    palette.setColor(QPalette::ToolTipBase, QColor(30, 36, 44));
    palette.setColor(QPalette::ToolTipText, QColor(230, 235, 243));
    palette.setColor(QPalette::Text, QColor(217, 222, 231));
    palette.setColor(QPalette::Button, QColor(31, 37, 45));
    palette.setColor(QPalette::ButtonText, QColor(217, 222, 231));
    palette.setColor(QPalette::BrightText, QColor(255, 255, 255));
    palette.setColor(QPalette::Link, QColor(75, 169, 255));
    palette.setColor(QPalette::Highlight, QColor(42, 124, 210));
    palette.setColor(QPalette::HighlightedText, QColor(255, 255, 255));
    palette.setColor(QPalette::Disabled, QPalette::Text, QColor(91, 99, 110));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(91, 99, 110));
    application.setPalette(palette);
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("VidScope"));
    QApplication::setOrganizationDomain(QStringLiteral("vidscope.app"));
    QApplication::setApplicationName(QStringLiteral("VidScope"));
    QApplication::setApplicationDisplayName(QStringLiteral("VidScope"));
    QApplication::setApplicationVersion(QStringLiteral("0.4.0"));

    vidscope::core::installLogging();
    vidscope::core::installFfmpegLogBridge();
    try {
        vidscope::media::ensureFfmpegNetworkInitialized();
    } catch (const std::exception& exception) {
        qCritical().noquote() << "Unable to initialize FFmpeg:" << exception.what();
        return EXIT_FAILURE;
    }
    installApplicationPalette(application);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Frame-accurate Qt and FFmpeg video inspection with a zoomable timeline and asynchronous hover previews."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption smokeTest(
        QStringLiteral("smoke-test"),
        QStringLiteral("Construct, exercise, and shut down the application for automated testing."));
    const QCommandLineOption mediaSmokeTest(
        QStringLiteral("media-smoke-test"),
        QStringLiteral("Open a video and exit after its first decoded image reaches the UI timeline."));
    parser.addOption(smokeTest);
    parser.addOption(mediaSmokeTest);
    parser.addPositionalArgument(QStringLiteral("video"), QStringLiteral("Video file to open."));
    parser.process(application);

    const bool runImmediateSmoke = parser.isSet(smokeTest);
    const bool runMediaSmoke = parser.isSet(mediaSmokeTest);
    const QStringList positional = parser.positionalArguments();

    if (runImmediateSmoke && runMediaSmoke) {
        qCritical() << "--smoke-test and --media-smoke-test cannot be used together";
        return EXIT_FAILURE;
    }
    if (runMediaSmoke && positional.size() != 1) {
        qCritical() << "--media-smoke-test requires exactly one video fixture";
        return EXIT_FAILURE;
    }

    vidscope::widgets::MainWindow window;
    window.show();

    auto* controller = window.findChild<vidscope::playback::PlaybackController*>(
        QStringLiteral("playbackController"));
    auto* timeline = window.findChild<vidscope::timeline::TimelineWidget*>(
        QStringLiteral("timelineWidget"));
    auto* thumbnailManager = window.findChild<vidscope::thumbnails::ThumbnailManager*>(
        QStringLiteral("thumbnailManager"));
    auto* hoverPreviewPopup = window.findChild<vidscope::widgets::HoverPreviewPopup*>(
        QStringLiteral("hoverPreviewPopup"));

    if ((runImmediateSmoke || runMediaSmoke)
        && (timeline == nullptr || thumbnailManager == nullptr || hoverPreviewPopup == nullptr)) {
        qCritical() << "Smoke test could not find the Phase 3/4 timeline preview components";
        return EXIT_FAILURE;
    }

    if (runMediaSmoke) {
        if (controller == nullptr) {
            qCritical() << "Media smoke test could not find the playback controller";
            return EXIT_FAILURE;
        }

        // Do not let the normal modal error dialog block an unattended offscreen test.
        QObject::disconnect(
            controller,
            &vidscope::playback::PlaybackController::errorOccurred,
            &window,
            nullptr);

        constexpr int mediaSmokeTimeoutMilliseconds = 20'000;
        QTimer mediaSmokeTimeout;
        mediaSmokeTimeout.setSingleShot(true);
        bool mediaSmokeFinished = false;

        QObject::connect(
            controller,
            &vidscope::playback::PlaybackController::frameReady,
            &application,
            [&application, &mediaSmokeTimeout, &mediaSmokeFinished, timeline](
                const vidscope::media::DecodedFramePtr& frame,
                const QImage& image) {
                if (mediaSmokeFinished || !frame || image.isNull()) {
                    return;
                }
                if (timeline == nullptr || !timeline->model().hasMedia()
                    || timeline->model().knownFrameCount() == 0) {
                    mediaSmokeFinished = true;
                    mediaSmokeTimeout.stop();
                    qCritical() << "Media smoke frame did not reach the Phase 3 timeline model";
                    application.exit(EXIT_FAILURE);
                    return;
                }

                mediaSmokeFinished = true;
                mediaSmokeTimeout.stop();
                application.exit(EXIT_SUCCESS);
            },
            Qt::QueuedConnection);
        QObject::connect(
            controller,
            &vidscope::playback::PlaybackController::errorOccurred,
            &application,
            [&application, &mediaSmokeTimeout, &mediaSmokeFinished](
                const QString& title,
                const QString& detail) {
                if (mediaSmokeFinished) {
                    return;
                }

                mediaSmokeFinished = true;
                mediaSmokeTimeout.stop();
                qCritical().noquote() << "Media smoke test failed:" << title << detail;
                application.exit(EXIT_FAILURE);
            },
            Qt::QueuedConnection);
        QObject::connect(
            &mediaSmokeTimeout,
            &QTimer::timeout,
            &application,
            [&application, &mediaSmokeFinished] {
                if (mediaSmokeFinished) {
                    return;
                }

                mediaSmokeFinished = true;
                qCritical() << "Media smoke test timed out waiting for a decoded UI image";
                application.exit(EXIT_FAILURE);
            });

        mediaSmokeTimeout.start(mediaSmokeTimeoutMilliseconds);
        controller->openFile(positional.constFirst());
        return application.exec();
    }

    if (!positional.isEmpty() && controller != nullptr) {
        controller->openFile(positional.constFirst());
    }

    if (runImmediateSmoke) {
        timeline->setDuration(vidscope::media::MediaTime{60s}.count());
        timeline->setPosition(vidscope::media::MediaTime{30s}.count());
        timeline->zoomIn();
        timeline->setInPointAtPlayhead();
        timeline->setPosition(vidscope::media::MediaTime{40s}.count());
        timeline->setOutPointAtPlayhead();
        const auto marker = timeline->addMarker(
            vidscope::media::MediaTime{35s}.count(),
            vidscope::timeline::TimelineMarkerKind::Bookmark,
            QStringLiteral("smoke"));
        if (!marker || !timeline->model().selection()
            || timeline->model().isShowingEntireMedia()) {
            qCritical() << "Phase 3 timeline smoke exercise failed";
            return EXIT_FAILURE;
        }
        QTimer::singleShot(0, &application, &QCoreApplication::quit);
    }

    return application.exec();
}
