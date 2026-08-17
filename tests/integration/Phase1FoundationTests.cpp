#include "TestHarness.h"

#include "media/MediaSource.h"
#include "playback/PlaybackController.h"
#include "render/VideoViewport.h"
#include "widgets/MainWindow.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QImage>
#include <QtWidgets/QApplication>
#include <QtWidgets/QLabel>

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

using namespace std::chrono_literals;

namespace {

using vidscope::media::DecodedFramePtr;
using vidscope::media::MediaInfoPtr;
using vidscope::media::MediaOpenOptions;
using vidscope::media::MediaSource;
using vidscope::playback::PlaybackController;
using vidscope::playback::PlaybackSessionConfig;
using vidscope::playback::PlaybackState;
using vidscope::render::VideoViewport;
using vidscope::widgets::MainWindow;

std::filesystem::path fixtureDirectory;

[[nodiscard]] QString pathToQString(const std::filesystem::path& path)
{
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

[[nodiscard]] std::filesystem::path fixturePath(const char* name)
{
    const auto path = fixtureDirectory / name;
    VIDSCOPE_REQUIRE_MESSAGE(std::filesystem::exists(path), path.string());
    return path;
}

template <typename Predicate>
bool waitUntil(Predicate&& predicate, int timeoutMilliseconds = 10'000)
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

template <typename Rep, typename Period>
[[nodiscard]] qint64 toNanoseconds(std::chrono::duration<Rep, Period> value)
{
    return static_cast<qint64>(
        std::chrono::duration_cast<vidscope::media::MediaTime>(value).count());
}

[[nodiscard]] PlaybackSessionConfig softwarePlaybackConfig()
{
    PlaybackSessionConfig config;
    config.frameCacheBytes = 32U * 1024U * 1024U;
    config.forwardQueueBytes = 16U * 1024U * 1024U;
    config.forwardQueueFrames = 8;
    config.initialPrefetchFrames = 4;
    config.decoder.hardwareAcceleration = vidscope::media::HardwareAcceleration::Disabled;
    return config;
}

struct ControllerProbe final {
    int openedCount = 0;
    int errorCount = 0;
    int frameCount = 0;
    MediaInfoPtr info;
    DecodedFramePtr frame;
    QImage image;
    QString lastError;
};

void connectProbe(PlaybackController& controller, ControllerProbe& probe)
{
    QObject::connect(
        &controller,
        &PlaybackController::mediaOpened,
        &controller,
        [&probe](MediaInfoPtr info) {
            ++probe.openedCount;
            probe.info = std::move(info);
        });
    QObject::connect(
        &controller,
        &PlaybackController::frameReady,
        &controller,
        [&probe](DecodedFramePtr frame, const QImage& image) {
            ++probe.frameCount;
            probe.frame = std::move(frame);
            probe.image = image;
        });
    QObject::connect(
        &controller,
        &PlaybackController::errorOccurred,
        &controller,
        [&probe](const QString& title, const QString& detail) {
            ++probe.errorCount;
            probe.lastError = title + QStringLiteral(": ") + detail;
        });
}

[[nodiscard]] QImage renderViewport(VideoViewport& viewport)
{
    VIDSCOPE_REQUIRE(viewport.width() > 0);
    VIDSCOPE_REQUIRE(viewport.height() > 0);
    QImage image(viewport.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    viewport.render(&image);
    return image;
}

} // namespace

VIDSCOPE_TEST(Phase1_MediaSource_selects_default_and_explicit_video_streams)
{
    const auto path = fixturePath("multi_stream.mkv");

    const auto defaultSource = MediaSource::open(path);
    VIDSCOPE_REQUIRE(defaultSource != nullptr);
    VIDSCOPE_REQUIRE(defaultSource->nativeHandle() != nullptr);
    VIDSCOPE_REQUIRE(defaultSource->nativeHandle()->nb_streams == 3);
    VIDSCOPE_REQUIRE(defaultSource->videoStreamIndex() == 0);
    VIDSCOPE_REQUIRE(defaultSource->info().width == 96);
    VIDSCOPE_REQUIRE(defaultSource->info().height == 54);
    VIDSCOPE_REQUIRE(defaultSource->videoStream() != nullptr);
    VIDSCOPE_REQUIRE(defaultSource->videoStream()->codecpar->codec_type == AVMEDIA_TYPE_VIDEO);

    MediaOpenOptions explicitVideo;
    explicitVideo.preferredVideoStream = 1;
    const auto secondVideo = MediaSource::open(path, explicitVideo);
    VIDSCOPE_REQUIRE(secondVideo != nullptr);
    VIDSCOPE_REQUIRE(secondVideo->videoStreamIndex() == 1);
    VIDSCOPE_REQUIRE(secondVideo->info().width == 160);
    VIDSCOPE_REQUIRE(secondVideo->info().height == 90);

    MediaOpenOptions audioStream;
    audioStream.preferredVideoStream = 2;
    bool rejectedAudioStream = false;
    try {
        static_cast<void>(MediaSource::open(path, audioStream));
    } catch (const std::invalid_argument&) {
        rejectedAudioStream = true;
    }
    VIDSCOPE_REQUIRE(rejectedAudioStream);
}

VIDSCOPE_TEST(Phase1_Controller_reports_open_errors_and_recovers_with_valid_media)
{
    ControllerProbe probe;
    auto controller = std::make_unique<PlaybackController>(softwarePlaybackConfig());
    connectProbe(*controller, probe);

    const auto missingPath = fixtureDirectory / "phase1-intentionally-missing.mp4";
    VIDSCOPE_REQUIRE(!std::filesystem::exists(missingPath));
    controller->openFile(pathToQString(missingPath));
    VIDSCOPE_REQUIRE(waitUntil([&] { return probe.errorCount >= 1; }));
    VIDSCOPE_REQUIRE(controller->state() == PlaybackState::Error);
    VIDSCOPE_REQUIRE(!probe.lastError.isEmpty());

    const int errorsBeforeAudio = probe.errorCount;
    controller->openFile(pathToQString(fixturePath("audio_only.mka")));
    VIDSCOPE_REQUIRE(waitUntil([&] { return probe.errorCount > errorsBeforeAudio; }));
    VIDSCOPE_REQUIRE(controller->state() == PlaybackState::Error);
    VIDSCOPE_REQUIRE(!probe.lastError.isEmpty());

    const int opensBeforeRecovery = probe.openedCount;
    const int framesBeforeRecovery = probe.frameCount;
    const int errorsBeforeRecovery = probe.errorCount;
    controller->openFile(pathToQString(fixturePath("cfr_no_b.mp4")));
    const bool recovered = waitUntil([&] {
        return probe.openedCount > opensBeforeRecovery
            && probe.frameCount > framesBeforeRecovery
            && probe.info != nullptr
            && probe.frame != nullptr
            && !probe.image.isNull()
            && controller->state() == PlaybackState::Stopped;
    });
    VIDSCOPE_REQUIRE_MESSAGE(recovered, probe.lastError.toStdString());
    VIDSCOPE_REQUIRE(probe.errorCount == errorsBeforeRecovery);
    VIDSCOPE_REQUIRE(probe.info->width == 160);
    VIDSCOPE_REQUIRE(probe.info->height == 90);
    VIDSCOPE_REQUIRE(probe.frame->id.presentationIndex == 0);

    QElapsedTimer shutdownTimer;
    shutdownTimer.start();
    controller.reset();
    VIDSCOPE_REQUIRE(shutdownTimer.elapsed() < 5'000);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}

VIDSCOPE_TEST(Phase1_Controller_open_play_pause_seek_stop_replay_and_shutdown)
{
    ControllerProbe probe;
    auto controller = std::make_unique<PlaybackController>(softwarePlaybackConfig());
    connectProbe(*controller, probe);

    controller->openFile(pathToQString(fixturePath("long_gop.mp4")));
    const bool opened = waitUntil([&] {
        return probe.info != nullptr
            && probe.frame != nullptr
            && !probe.image.isNull()
            && controller->state() == PlaybackState::Stopped;
    });
    VIDSCOPE_REQUIRE_MESSAGE(opened, probe.lastError.toStdString());
    VIDSCOPE_REQUIRE(probe.frame->id.presentationIndex == 0);
    VIDSCOPE_REQUIRE(probe.frame->presentationTime == 0ns);

    const int framesBeforePlay = probe.frameCount;
    controller->play();
    const bool played = waitUntil([&] {
        return probe.frameCount > framesBeforePlay
            && probe.frame != nullptr
            && probe.frame->presentationTime > 0ns
            && controller->state() == PlaybackState::Playing;
    });
    VIDSCOPE_REQUIRE(played);

    controller->pause();
    VIDSCOPE_REQUIRE(waitUntil([&] { return controller->state() == PlaybackState::Paused; }));

    constexpr auto seekTarget = 2'350ms;
    const int framesBeforeSeek = probe.frameCount;
    controller->seekToNanoseconds(toNanoseconds(seekTarget));
    const bool sought = waitUntil([&] {
        return probe.frameCount > framesBeforeSeek
            && probe.frame != nullptr
            && probe.frame->presentationTime >= seekTarget
            && controller->state() == PlaybackState::Paused;
    });
    VIDSCOPE_REQUIRE(sought);
    VIDSCOPE_REQUIRE(probe.frame->presentationTime == 2'400ms);

    const int framesBeforeStop = probe.frameCount;
    controller->stop();
    const bool stoppedAtFirstFrame = waitUntil([&] {
        return probe.frameCount > framesBeforeStop
            && probe.frame != nullptr
            && probe.frame->presentationTime == 0ns
            && controller->state() == PlaybackState::Stopped;
    });
    VIDSCOPE_REQUIRE(stoppedAtFirstFrame);
    VIDSCOPE_REQUIRE(probe.frame->id.presentationIndex == 0);

    const int framesBeforeReplay = probe.frameCount;
    controller->play();
    const bool replayed = waitUntil([&] {
        return probe.frameCount > framesBeforeReplay
            && probe.frame != nullptr
            && probe.frame->presentationTime > 0ns
            && controller->state() == PlaybackState::Playing;
    });
    VIDSCOPE_REQUIRE(replayed);

    for (int request = 0; request < 24; ++request) {
        controller->seekToNanoseconds(
            request % 2 == 0 ? toNanoseconds(500ms) : toNanoseconds(5'500ms));
    }
    QPointer<PlaybackController> guard(controller.get());
    QElapsedTimer shutdownTimer;
    shutdownTimer.start();
    controller.reset();
    VIDSCOPE_REQUIRE(shutdownTimer.elapsed() < 5'000);
    VIDSCOPE_REQUIRE(guard.isNull());
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}

VIDSCOPE_TEST(Phase1_MainWindow_publishes_metadata_actions_and_rendered_video)
{
    auto window = std::make_unique<MainWindow>();
    window->resize(1'100, 760);
    window->show();
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    auto* controller = window->findChild<PlaybackController*>(
        QStringLiteral("playbackController"));
    auto* viewport = window->findChild<VideoViewport*>(QStringLiteral("videoViewport"));
    auto* mediaStatus = window->findChild<QLabel*>(QStringLiteral("mediaStatus"));
    auto* frameStatus = window->findChild<QLabel*>(QStringLiteral("frameStatus"));
    auto* position = window->findChild<QLabel*>(QStringLiteral("positionLabel"));
    auto* openAction = window->findChild<QAction*>(QStringLiteral("actionOpen"));
    auto* playPauseAction = window->findChild<QAction*>(QStringLiteral("actionPlayPause"));
    auto* pauseAction = window->findChild<QAction*>(QStringLiteral("actionPause"));
    auto* stopAction = window->findChild<QAction*>(QStringLiteral("actionStop"));

    VIDSCOPE_REQUIRE(controller != nullptr);
    VIDSCOPE_REQUIRE(viewport != nullptr);
    VIDSCOPE_REQUIRE(mediaStatus != nullptr);
    VIDSCOPE_REQUIRE(frameStatus != nullptr);
    VIDSCOPE_REQUIRE(position != nullptr);
    VIDSCOPE_REQUIRE(openAction != nullptr);
    VIDSCOPE_REQUIRE(playPauseAction != nullptr);
    VIDSCOPE_REQUIRE(pauseAction != nullptr);
    VIDSCOPE_REQUIRE(stopAction != nullptr);
    VIDSCOPE_REQUIRE(openAction->isEnabled());
    VIDSCOPE_REQUIRE(!playPauseAction->isEnabled());

    const QImage emptyViewport = renderViewport(*viewport);

    // The production handler uses a modal dialog. Disconnect it so an
    // unexpected error is reported by this unattended offscreen test instead.
    QObject::disconnect(
        controller,
        &PlaybackController::errorOccurred,
        window.get(),
        nullptr);
    QString error;
    QObject::connect(
        controller,
        &PlaybackController::errorOccurred,
        window.get(),
        [&error](const QString& title, const QString& detail) {
            error = title + QStringLiteral(": ") + detail;
        });

    controller->openFile(pathToQString(fixturePath("multi_stream.mkv")));
    const bool populated = waitUntil([&] {
        return !error.isEmpty()
            || (mediaStatus->text().contains(QStringLiteral("multi_stream.mkv"))
                && mediaStatus->text().contains(QStringLiteral("96x54"))
                && frameStatus->text().startsWith(QStringLiteral("Frame 0 |"))
                && playPauseAction->isEnabled()
                && pauseAction->isEnabled()
                && stopAction->isEnabled());
    }, 15'000);
    VIDSCOPE_REQUIRE_MESSAGE(error.isEmpty(), error.toStdString());
    VIDSCOPE_REQUIRE(populated);
    VIDSCOPE_REQUIRE(mediaStatus->toolTip().contains(QStringLiteral("Container:")));
    VIDSCOPE_REQUIRE(mediaStatus->toolTip().contains(QStringLiteral("Codec:")));
    VIDSCOPE_REQUIRE(mediaStatus->toolTip().contains(QStringLiteral("Duration:")));
    VIDSCOPE_REQUIRE(frameStatus->text().contains(QStringLiteral("PTS")));
    VIDSCOPE_REQUIRE(frameStatus->toolTip().contains(QStringLiteral("Dimensions: 96x54")));
    VIDSCOPE_REQUIRE(frameStatus->toolTip().contains(QStringLiteral("Bit depth:")));
    VIDSCOPE_REQUIRE(position->property("durationNs").toLongLong() > 0);

    const QImage populatedViewport = renderViewport(*viewport);
    VIDSCOPE_REQUIRE(emptyViewport != populatedViewport);

    QPointer<MainWindow> windowGuard(window.get());
    QPointer<PlaybackController> controllerGuard(controller);
    QElapsedTimer shutdownTimer;
    shutdownTimer.start();
    window->close();
    window.reset();
    VIDSCOPE_REQUIRE(shutdownTimer.elapsed() < 5'000);
    VIDSCOPE_REQUIRE(windowGuard.isNull());
    VIDSCOPE_REQUIRE(controllerGuard.isNull());
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
    QApplication::setApplicationName(QStringLiteral("Phase1FoundationTests"));
    application.setQuitOnLastWindowClosed(false);

    QTemporaryDir settingsDirectory;
    if (!settingsDirectory.isValid()) {
        return 3;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::UserScope,
        settingsDirectory.path());
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::SystemScope,
        settingsDirectory.path());

    fixtureDirectory = std::filesystem::path(argv[1]);
    return vidscope::test::runAll();
}
