#include "TestHarness.h"

#include "playback/PlaybackController.h"
#include "playback/PlaybackSession.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QThread>
#include <QtCore/QTimer>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

using namespace std::chrono_literals;

namespace {

std::filesystem::path fixtureDirectory;

vidscope::playback::PlaybackSessionConfig tinyCacheConfig()
{
    vidscope::playback::PlaybackSessionConfig config;
    config.frameCacheBytes = 1;
    config.forwardQueueBytes = 1;
    config.forwardQueueFrames = 1;
    config.initialPrefetchFrames = 0;
    config.decoder.hardwareAcceleration = vidscope::media::HardwareAcceleration::Disabled;
    return config;
}

vidscope::playback::PlaybackSessionConfig controllerConfig()
{
    vidscope::playback::PlaybackSessionConfig config;
    config.frameCacheBytes = 16U * 1024U * 1024U;
    config.forwardQueueBytes = 8U * 1024U * 1024U;
    config.forwardQueueFrames = 4;
    config.initialPrefetchFrames = 2;
    config.decoder.hardwareAcceleration = vidscope::media::HardwareAcceleration::Disabled;
    return config;
}

QString fixturePath(const char* name)
{
#ifdef Q_OS_WIN
    return QString::fromStdWString((fixtureDirectory / name).wstring());
#else
    return QString::fromStdString((fixtureDirectory / name).string());
#endif
}

qint64 toNanoseconds(std::chrono::nanoseconds value)
{
    return static_cast<qint64>(value.count());
}

bool openController(
    vidscope::playback::PlaybackController& controller,
    const char* fixture,
    QString& error)
{
    bool opened = false;
    bool timedOut = false;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        loop.quit();
    });
    QObject::connect(
        &controller,
        &vidscope::playback::PlaybackController::mediaOpened,
        &loop,
        [&](vidscope::media::MediaInfoPtr) {
            opened = true;
            loop.quit();
        });
    QObject::connect(
        &controller,
        &vidscope::playback::PlaybackController::errorOccurred,
        &loop,
        [&](const QString& title, const QString& detail) {
            error = title + QStringLiteral(": ") + detail;
            loop.quit();
        });

    timeout.start(10'000);
    controller.openFile(fixturePath(fixture));
    loop.exec();
    if (timedOut) {
        error = QStringLiteral("Timed out while opening the fixture.");
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    return opened && error.isEmpty();
}

vidscope::media::DecodedFramePtr stepController(
    vidscope::playback::PlaybackController& controller,
    int frameCount,
    bool& timedOut)
{
    vidscope::media::DecodedFramePtr deliveredFrame;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        loop.quit();
    });
    QObject::connect(
        &controller,
        &vidscope::playback::PlaybackController::frameReady,
        &loop,
        [&](vidscope::media::DecodedFramePtr frame, const QImage&) {
            if (frame) {
                deliveredFrame = std::move(frame);
                loop.quit();
            }
        });

    timedOut = false;
    timeout.start(10'000);
    controller.stepFrames(frameCount);
    loop.exec();
    return deliveredFrame;
}

} // namespace

VIDSCOPE_TEST(Previous_frame_reconstructs_exactly_when_no_decoded_frame_fits_the_cache)
{
    vidscope::playback::PlaybackSession session(tinyCacheConfig());
    VIDSCOPE_REQUIRE(static_cast<bool>(session.open(fixtureDirectory / "long_gop.mp4")));

    const auto sought = session.seek(
        {1, 3'050ms, vidscope::playback::SeekBias::AtOrAfter});
    VIDSCOPE_REQUIRE(static_cast<bool>(sought));
    VIDSCOPE_REQUIRE(sought.frame->presentationTime == 3'100ms);
    VIDSCOPE_REQUIRE(session.cacheStats().bytes <= 1);

    const auto previous = session.previousFrame();
    VIDSCOPE_REQUIRE(static_cast<bool>(previous));
    VIDSCOPE_REQUIRE(previous.frame->presentationTime == 3'000ms);

    const auto next = session.nextFrame();
    VIDSCOPE_REQUIRE(static_cast<bool>(next));
    VIDSCOPE_REQUIRE(next.frame->presentationTime == sought.frame->presentationTime);

    const auto previousAgain = session.previousFrame();
    VIDSCOPE_REQUIRE(static_cast<bool>(previousAgain));
    VIDSCOPE_REQUIRE(previousAgain.frame->presentationTime == previous.frame->presentationTime);
    VIDSCOPE_REQUIRE(session.cacheStats().bytes <= 1);
}

VIDSCOPE_TEST(Failed_next_keyframe_scan_preserves_the_published_frame)
{
    vidscope::playback::PlaybackSession session(tinyCacheConfig());
    VIDSCOPE_REQUIRE(static_cast<bool>(session.open(fixtureDirectory / "long_gop.mp4")));
    const auto sought = session.seek(
        {2, 5'500ms, vidscope::playback::SeekBias::AtOrAfter});
    VIDSCOPE_REQUIRE(static_cast<bool>(sought));
    const auto originalTime = sought.frame->presentationTime;

    const auto result = session.nextKeyframe();
    VIDSCOPE_REQUIRE(result.status == vidscope::playback::NavigationStatus::EndOfStream);
    VIDSCOPE_REQUIRE(session.currentFrame() != nullptr);
    VIDSCOPE_REQUIRE(session.currentFrame()->presentationTime == originalTime);
}

VIDSCOPE_TEST(Controller_signed_frame_step_lands_on_the_exact_presentation_index)
{
    auto controller = std::make_unique<vidscope::playback::PlaybackController>(
        controllerConfig());
    QString error;
    VIDSCOPE_REQUIRE_MESSAGE(
        openController(*controller, "cfr_bframes.mp4", error),
        error.toStdString());

    bool timedOut = false;
    const auto forward = stepController(*controller, 10, timedOut);
    VIDSCOPE_REQUIRE(!timedOut);
    VIDSCOPE_REQUIRE(forward != nullptr);
    VIDSCOPE_REQUIRE(forward->id.presentationIndex == 10);

    const auto backward = stepController(*controller, -5, timedOut);
    VIDSCOPE_REQUIRE(!timedOut);
    VIDSCOPE_REQUIRE(backward != nullptr);
    VIDSCOPE_REQUIRE(backward->id.presentationIndex == 5);
}

VIDSCOPE_TEST(Controller_failed_next_keyframe_stays_paused_and_play_resumes_at_successor)
{
    auto controller = std::make_unique<vidscope::playback::PlaybackController>(
        controllerConfig());
    QString error;
    VIDSCOPE_REQUIRE_MESSAGE(
        openController(*controller, "long_gop.mp4", error),
        error.toStdString());

    vidscope::media::DecodedFramePtr soughtFrame;
    bool seekTimedOut = false;
    QEventLoop seekLoop;
    QTimer seekTimeout;
    seekTimeout.setSingleShot(true);
    QObject::connect(&seekTimeout, &QTimer::timeout, &seekLoop, [&] {
        seekTimedOut = true;
        seekLoop.quit();
    });
    QObject::connect(
        controller.get(),
        &vidscope::playback::PlaybackController::frameReady,
        &seekLoop,
        [&](vidscope::media::DecodedFramePtr frame, const QImage&) {
            if (frame && frame->presentationTime == 5'500ms) {
                soughtFrame = std::move(frame);
                seekLoop.quit();
            }
        });

    seekTimeout.start(10'000);
    controller->seekToNanoseconds(toNanoseconds(5'500ms));
    seekLoop.exec();
    VIDSCOPE_REQUIRE(!seekTimedOut);
    VIDSCOPE_REQUIRE(soughtFrame != nullptr);

    bool sawEnded = false;
    bool playbackTimedOut = false;
    vidscope::media::DecodedFramePtr firstPlaybackFrame;
    QEventLoop playbackLoop;
    QTimer playbackTimeout;
    playbackTimeout.setSingleShot(true);
    QObject::connect(&playbackTimeout, &QTimer::timeout, &playbackLoop, [&] {
        playbackTimedOut = true;
        playbackLoop.quit();
    });
    QObject::connect(
        controller.get(),
        &vidscope::playback::PlaybackController::stateChanged,
        &playbackLoop,
        [&](vidscope::playback::PlaybackState state) {
            sawEnded = sawEnded || state == vidscope::playback::PlaybackState::Ended;
        });
    QObject::connect(
        controller.get(),
        &vidscope::playback::PlaybackController::frameReady,
        &playbackLoop,
        [&](vidscope::media::DecodedFramePtr frame, const QImage&) {
            if (frame) {
                firstPlaybackFrame = std::move(frame);
                playbackLoop.quit();
            }
        });

    controller->nextKeyframe();
    controller->play();
    playbackTimeout.start(10'000);
    playbackLoop.exec();
    VIDSCOPE_REQUIRE(!playbackTimedOut);
    VIDSCOPE_REQUIRE(!sawEnded);
    VIDSCOPE_REQUIRE(firstPlaybackFrame != nullptr);
    VIDSCOPE_REQUIRE(firstPlaybackFrame->presentationTime == 5'600ms);
}

VIDSCOPE_TEST(Controller_coalesces_frame_and_metrics_delivery_while_the_GUI_thread_is_stalled)
{
    auto controller = std::make_unique<vidscope::playback::PlaybackController>(
        controllerConfig());
    QString error;
    VIDSCOPE_REQUIRE_MESSAGE(
        openController(*controller, "long_gop.mp4", error),
        error.toStdString());

    int deliveredFrames = 0;
    int deliveredMetrics = 0;
    vidscope::media::MediaTime latestTime{};
    QObject receiver;
    QObject::connect(
        controller.get(),
        &vidscope::playback::PlaybackController::frameReady,
        &receiver,
        [&](vidscope::media::DecodedFramePtr frame, const QImage&) {
            ++deliveredFrames;
            if (frame) {
                latestTime = frame->presentationTime;
            }
        });
    QObject::connect(
        controller.get(),
        &vidscope::playback::PlaybackController::metricsUpdated,
        &receiver,
        [&](double, qint64, qsizetype) { ++deliveredMetrics; });

    controller->play();
    QElapsedTimer stateTimer;
    stateTimer.start();
    while (controller->state() != vidscope::playback::PlaybackState::Playing
           && stateTimer.elapsed() < 2'000) {
        QThread::msleep(2);
    }
    VIDSCOPE_REQUIRE(controller->state() == vidscope::playback::PlaybackState::Playing);

    QThread::msleep(750);
    controller->pause();
    stateTimer.restart();
    while (controller->state() == vidscope::playback::PlaybackState::Playing
           && stateTimer.elapsed() < 2'000) {
        QThread::msleep(2);
    }
    VIDSCOPE_REQUIRE(controller->state() == vidscope::playback::PlaybackState::Paused);

    QCoreApplication::processEvents(QEventLoop::AllEvents);
    VIDSCOPE_REQUIRE(deliveredFrames == 1);
    VIDSCOPE_REQUIRE(deliveredMetrics == deliveredFrames);
    VIDSCOPE_REQUIRE(latestTime >= 400ms);
}

VIDSCOPE_TEST(Controller_coalesces_rapid_seeks_and_joins_its_worker_deterministically)
{
    auto controller = std::make_unique<vidscope::playback::PlaybackController>(
        controllerConfig());

    QString error;
    bool opened = false;
    bool openTimedOut = false;
    QEventLoop openLoop;
    QTimer openTimeout;
    openTimeout.setSingleShot(true);
    QObject::connect(&openTimeout, &QTimer::timeout, &openLoop, [&] {
        openTimedOut = true;
        openLoop.quit();
    });
    QObject::connect(
        controller.get(),
        &vidscope::playback::PlaybackController::mediaOpened,
        &openLoop,
        [&](vidscope::media::MediaInfoPtr) {
            opened = true;
            openLoop.quit();
        });
    QObject::connect(
        controller.get(),
        &vidscope::playback::PlaybackController::errorOccurred,
        &openLoop,
        [&](const QString& title, const QString& detail) {
            error = title + QStringLiteral(": ") + detail;
            openLoop.quit();
        });

    openTimeout.start(10'000);
    controller->openFile(fixturePath("long_gop.mp4"));
    openLoop.exec();
    VIDSCOPE_REQUIRE(!openTimedOut);
    VIDSCOPE_REQUIRE_MESSAGE(error.isEmpty(), error.toStdString());
    VIDSCOPE_REQUIRE(opened);
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    constexpr auto latestTarget = 4'300ms;
    bool gotLatest = false;
    bool gotStaleFrame = false;
    bool seekTimedOut = false;
    QEventLoop seekLoop;
    QTimer seekTimeout;
    seekTimeout.setSingleShot(true);
    QObject::connect(&seekTimeout, &QTimer::timeout, &seekLoop, [&] {
        seekTimedOut = true;
        seekLoop.quit();
    });
    QObject::connect(
        controller.get(),
        &vidscope::playback::PlaybackController::frameReady,
        &seekLoop,
        [&](vidscope::media::DecodedFramePtr frame, const QImage&) {
            if (!frame) {
                return;
            }
            if (frame->presentationTime < latestTarget) {
                gotStaleFrame = true;
            }
            if (frame->presentationTime == latestTarget) {
                gotLatest = true;
                seekLoop.quit();
            }
        });

    controller->seekToNanoseconds(toNanoseconds(1'100ms));
    controller->seekToNanoseconds(toNanoseconds(2'700ms));
    controller->seekToNanoseconds(toNanoseconds(latestTarget));
    seekTimeout.start(10'000);
    seekLoop.exec();
    VIDSCOPE_REQUIRE(!seekTimedOut);
    VIDSCOPE_REQUIRE(gotLatest);
    VIDSCOPE_REQUIRE(!gotStaleFrame);

    controller->play();
    for (int request = 0; request < 32; ++request) {
        controller->seekToNanoseconds(toNanoseconds(
            request % 2 == 0 ? std::chrono::milliseconds{500}
                             : std::chrono::milliseconds{5'500}));
    }
    QElapsedTimer shutdownTimer;
    shutdownTimer.start();
    controller.reset();
    VIDSCOPE_REQUIRE(shutdownTimer.elapsed() < 5'000);
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 2) {
        return 2;
    }
    fixtureDirectory = std::filesystem::path(argv[1]);
    return vidscope::test::runAll();
}
