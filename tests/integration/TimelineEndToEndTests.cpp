#include "TestHarness.h"

#include "playback/PlaybackController.h"
#include "playback/PlaybackSession.h"
#include "timeline/TimelineWidget.h"
#include "widgets/MainWindow.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>
#include <QtTest/QSignalSpy>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

using vidscope::media::DecodedFramePtr;
using vidscope::media::MediaTime;
using vidscope::playback::NavigationStatus;
using vidscope::playback::PlaybackController;
using vidscope::playback::PlaybackSession;
using vidscope::timeline::TimelineWidget;
using vidscope::widgets::MainWindow;

constexpr qreal kTrackInset = 12.0;
std::filesystem::path fixtureDirectory;

struct ReferenceVideo final {
    MediaTime duration{};
    std::vector<DecodedFramePtr> frames;
};

vidscope::playback::PlaybackSessionConfig softwareConfig()
{
    vidscope::playback::PlaybackSessionConfig config;
    config.frameCacheBytes = 64U * 1024U * 1024U;
    config.forwardQueueBytes = 32U * 1024U * 1024U;
    config.forwardQueueFrames = 12;
    config.initialPrefetchFrames = 6;
    config.decoder.hardwareAcceleration = vidscope::media::HardwareAcceleration::Disabled;
    return config;
}

ReferenceVideo decodeReference(const std::filesystem::path& path)
{
    PlaybackSession session(softwareConfig());
    const auto opened = session.open(path);
    VIDSCOPE_REQUIRE_MESSAGE(static_cast<bool>(opened), path.string());
    VIDSCOPE_REQUIRE(opened.frame != nullptr);
    VIDSCOPE_REQUIRE(session.mediaInfo() != nullptr);

    ReferenceVideo reference;
    reference.duration = session.mediaInfo()->duration;
    reference.frames.push_back(opened.frame);
    for (std::size_t guard = 0; guard < 10'000; ++guard) {
        const auto next = session.nextFrame();
        if (next.status == NavigationStatus::EndOfStream) {
            session.close();
            return reference;
        }
        VIDSCOPE_REQUIRE(next.status == NavigationStatus::FrameReady);
        VIDSCOPE_REQUIRE(next.frame != nullptr);
        VIDSCOPE_REQUIRE(
            next.frame->presentationTime > reference.frames.back()->presentationTime);
        reference.frames.push_back(next.frame);
    }

    vidscope::test::fail(
        "PlaybackSession reaches end of stream",
        __FILE__,
        __LINE__,
        path.string());
}

[[nodiscard]] QString pathToQString(const std::filesystem::path& path)
{
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

template <typename Predicate>
bool waitUntil(Predicate&& predicate, int timeoutMilliseconds)
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

void processEventsFor(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

[[nodiscard]] QPointF timelinePoint(const TimelineWidget& timeline, qreal x)
{
    return {x, static_cast<qreal>(timeline.height()) / 2.0};
}

void sendMouse(
    TimelineWidget& timeline,
    QEvent::Type type,
    const QPointF& localPosition,
    Qt::MouseButton button,
    Qt::MouseButtons buttons)
{
    const QPointF globalPosition(timeline.mapToGlobal(localPosition.toPoint()));
    QMouseEvent event(
        type,
        localPosition,
        globalPosition,
        button,
        buttons,
        Qt::NoModifier);
    QCoreApplication::sendEvent(&timeline, &event);
}

void clickTimeline(TimelineWidget& timeline, qreal x)
{
    const auto point = timelinePoint(timeline, x);
    sendMouse(
        timeline,
        QEvent::MouseButtonPress,
        point,
        Qt::LeftButton,
        Qt::LeftButton);
    sendMouse(
        timeline,
        QEvent::MouseButtonRelease,
        point,
        Qt::LeftButton,
        Qt::NoButton);
}

[[nodiscard]] qreal xForTime(const TimelineWidget& timeline, MediaTime time)
{
    const qreal width = std::max<qreal>(
        1.0,
        static_cast<qreal>(timeline.width()) - 2.0 * kTrackInset);
    return timeline.model().timeToPixel(time, kTrackInset, width);
}

[[nodiscard]] MediaTime midpointTarget(
    const ReferenceVideo& reference,
    std::size_t expectedFrameIndex)
{
    VIDSCOPE_REQUIRE(expectedFrameIndex > 0);
    VIDSCOPE_REQUIRE(expectedFrameIndex < reference.frames.size());
    const auto before = reference.frames[expectedFrameIndex - 1]->presentationTime;
    const auto after = reference.frames[expectedFrameIndex]->presentationTime;
    VIDSCOPE_REQUIRE(before < after);
    return before + (after - before) / 2;
}

[[nodiscard]] const DecodedFramePtr& expectedFrameAtOrAfter(
    const ReferenceVideo& reference,
    qint64 requestedNanoseconds)
{
    const auto requested = MediaTime(requestedNanoseconds);
    const auto found = std::lower_bound(
        reference.frames.cbegin(),
        reference.frames.cend(),
        requested,
        [](const DecodedFramePtr& frame, MediaTime target) {
            return frame->presentationTime < target;
        });
    VIDSCOPE_REQUIRE(found != reference.frames.cend());
    return *found;
}

void requireExactFrame(
    const DecodedFramePtr& actual,
    const DecodedFramePtr& expected)
{
    VIDSCOPE_REQUIRE(actual != nullptr);
    VIDSCOPE_REQUIRE(expected != nullptr);
    VIDSCOPE_REQUIRE(
        actual->id.presentationIndex == expected->id.presentationIndex);
    VIDSCOPE_REQUIRE(actual->presentationTime == expected->presentationTime);

    const bool compatibleSurfaces = actual->storage != nullptr
        && expected->storage != nullptr
        && actual->width == expected->width
        && actual->height == expected->height
        && actual->pixelFormat == expected->pixelFormat;
    if (compatibleSurfaces) {
        VIDSCOPE_REQUIRE(vidscope::media::visibleImagesEqual(*actual, *expected));
    }
}

struct WindowObjects final {
    PlaybackController* controller = nullptr;
    TimelineWidget* timeline = nullptr;
};

WindowObjects findWindowObjects(MainWindow& window)
{
    WindowObjects objects;
    objects.controller = window.findChild<PlaybackController*>(
        QStringLiteral("playbackController"));
    objects.timeline = window.findChild<TimelineWidget*>(
        QStringLiteral("timelineWidget"));
    VIDSCOPE_REQUIRE(objects.controller != nullptr);
    VIDSCOPE_REQUIRE(objects.timeline != nullptr);
    return objects;
}

} // namespace

VIDSCOPE_TEST(Timeline_end_to_end_VFR_click_and_latest_burst_deliver_exact_frames)
{
    const auto fixture = fixtureDirectory / "vfr.mp4";
    VIDSCOPE_REQUIRE_MESSAGE(std::filesystem::exists(fixture), fixture.string());
    const auto reference = decodeReference(fixture);
    VIDSCOPE_REQUIRE(reference.frames.size() >= 18);

    auto window = std::make_unique<MainWindow>();
    window->resize(1'200, 800);
    window->show();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    const auto objects = findWindowObjects(*window);

    QObject::disconnect(
        objects.controller,
        &PlaybackController::errorOccurred,
        window.get(),
        nullptr);
    QString error;
    QObject::connect(
        objects.controller,
        &PlaybackController::errorOccurred,
        window.get(),
        [&](const QString& title, const QString& detail) {
            error = title + QStringLiteral(": ") + detail;
        });

    std::vector<DecodedFramePtr> delivered;
    QObject::connect(
        objects.controller,
        &PlaybackController::frameReady,
        window.get(),
        [&](DecodedFramePtr frame, const QImage&) {
            if (frame) {
                delivered.push_back(std::move(frame));
            }
        });

    objects.controller->openFile(pathToQString(fixture));
    const bool opened = waitUntil(
        [&] {
            return !error.isEmpty()
                || (objects.timeline->model().hasMedia()
                    && objects.timeline->model().knownFrameCount() >= 1);
        },
        15'000);
    VIDSCOPE_REQUIRE_MESSAGE(error.isEmpty(), error.toStdString());
    VIDSCOPE_REQUIRE(opened);
    VIDSCOPE_REQUIRE(objects.timeline->model().duration() == reference.duration);
    processEventsFor(75);
    delivered.clear();

    QSignalSpy seekSpy(objects.timeline, &TimelineWidget::seekRequested);
    VIDSCOPE_REQUIRE(seekSpy.isValid());

    // Frame 6 is not a keyframe in the generated VFR fixture. Clicking the
    // midpoint before it exercises AtOrAfter selection without marker snap.
    const auto controlledTarget = midpointTarget(reference, 6);
    VIDSCOPE_REQUIRE(!reference.frames[6]->keyFrame);
    clickTimeline(*objects.timeline, xForTime(*objects.timeline, controlledTarget));
    VIDSCOPE_REQUIRE(seekSpy.count() == 1);
    const qint64 requested = seekSpy.at(seekSpy.count() - 1).at(0).toLongLong();
    VIDSCOPE_REQUIRE(MediaTime(requested) != reference.frames[6]->presentationTime);
    const auto& controlledExpected = expectedFrameAtOrAfter(reference, requested);

    const bool controlledDelivered = waitUntil(
        [&] { return !error.isEmpty() || !delivered.empty(); },
        15'000);
    VIDSCOPE_REQUIRE_MESSAGE(error.isEmpty(), error.toStdString());
    VIDSCOPE_REQUIRE(controlledDelivered);
    VIDSCOPE_REQUIRE(!delivered.empty());
    requireExactFrame(delivered.front(), controlledExpected);

    processEventsFor(150);
    delivered.clear();
    seekSpy.clear();

    const auto targetA = midpointTarget(reference, 4);
    const auto targetB = midpointTarget(reference, 9);
    const auto targetC = midpointTarget(reference, 15);

    // sendEvent is synchronous and no event loop is entered between these
    // clicks, so queued worker deliveries cannot interleave with A -> B -> C.
    clickTimeline(*objects.timeline, xForTime(*objects.timeline, targetA));
    VIDSCOPE_REQUIRE(seekSpy.count() == 1);
    const qint64 requestedA = seekSpy.at(seekSpy.count() - 1).at(0).toLongLong();
    clickTimeline(*objects.timeline, xForTime(*objects.timeline, targetB));
    VIDSCOPE_REQUIRE(seekSpy.count() == 2);
    const qint64 requestedB = seekSpy.at(seekSpy.count() - 1).at(0).toLongLong();
    clickTimeline(*objects.timeline, xForTime(*objects.timeline, targetC));
    VIDSCOPE_REQUIRE(seekSpy.count() == 3);
    const qint64 requestedC = seekSpy.at(seekSpy.count() - 1).at(0).toLongLong();

    const auto& expectedA = expectedFrameAtOrAfter(reference, requestedA);
    const auto& expectedB = expectedFrameAtOrAfter(reference, requestedB);
    const auto& expectedC = expectedFrameAtOrAfter(reference, requestedC);
    VIDSCOPE_REQUIRE(
        expectedA->id.presentationIndex != expectedC->id.presentationIndex);
    VIDSCOPE_REQUIRE(
        expectedB->id.presentationIndex != expectedC->id.presentationIndex);

    const bool burstDelivered = waitUntil(
        [&] { return !error.isEmpty() || !delivered.empty(); },
        15'000);
    VIDSCOPE_REQUIRE_MESSAGE(error.isEmpty(), error.toStdString());
    VIDSCOPE_REQUIRE(burstDelivered);
    VIDSCOPE_REQUIRE(!delivered.empty());
    requireExactFrame(delivered.front(), expectedC);

    // Allow any already-posted result to run; every post-burst delivery must
    // still represent C. This catches stale A/B overwriting the final seek.
    processEventsFor(300);
    VIDSCOPE_REQUIRE(!delivered.empty());
    for (const auto& frame : delivered) {
        requireExactFrame(frame, expectedC);
    }

    QPointer<MainWindow> guard(window.get());
    window->close();
    window.reset();
    VIDSCOPE_REQUIRE(guard.isNull());
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
    QApplication::setApplicationName(QStringLiteral("TimelineEndToEndTests"));
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
