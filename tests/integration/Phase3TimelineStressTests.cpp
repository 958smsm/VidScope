#include "TestHarness.h"

#include "playback/PlaybackController.h"
#include "timeline/TimelineWidget.h"
#include "widgets/MainWindow.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QPointer>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtTest/QSignalSpy>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

using namespace std::chrono_literals;

namespace {

using vidscope::playback::PlaybackController;
using vidscope::timeline::TimelineWidget;
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

void openVfrFixture(
    MainWindow& window,
    PlaybackController& controller,
    TimelineWidget& timeline,
    std::size_t minimumKnownFrames)
{
    const auto fixture = fixtureDirectory / "vfr.mp4";
    VIDSCOPE_REQUIRE_MESSAGE(std::filesystem::exists(fixture), fixture.string());

    // Prevent the normal modal error dialog from blocking an unattended
    // offscreen test. The test records and reports the same error signal.
    QObject::disconnect(
        &controller,
        &PlaybackController::errorOccurred,
        &window,
        nullptr);

    QString error;
    const auto errorConnection = QObject::connect(
        &controller,
        &PlaybackController::errorOccurred,
        &window,
        [&](const QString& title, const QString& detail) {
            error = title + QStringLiteral(": ") + detail;
        });

    controller.openFile(pathToQString(fixture));
    const bool populated = waitUntil(
        [&] {
            return !error.isEmpty()
                || (timeline.model().hasMedia()
                    && timeline.model().knownFrameCount() >= minimumKnownFrames);
        },
        15'000);
    QObject::disconnect(errorConnection);

    VIDSCOPE_REQUIRE_MESSAGE(error.isEmpty(), error.toStdString());
    VIDSCOPE_REQUIRE(populated);
    VIDSCOPE_REQUIRE(timeline.model().duration() > vidscope::media::MediaTime::zero());
    VIDSCOPE_REQUIRE(timeline.model().knownFrameCount() >= minimumKnownFrames);
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
    Qt::MouseButtons buttons,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const QPointF globalPosition(timeline.mapToGlobal(localPosition.toPoint()));
    QMouseEvent event(
        type,
        localPosition,
        globalPosition,
        button,
        buttons,
        modifiers);
    QCoreApplication::sendEvent(&timeline, &event);
}

void sendWheel(TimelineWidget& timeline, const QPointF& localPosition, int angleDeltaY)
{
    const QPointF globalPosition(timeline.mapToGlobal(localPosition.toPoint()));
    QWheelEvent event(
        localPosition,
        globalPosition,
        {},
        QPoint(0, angleDeltaY),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::NoScrollPhase,
        false);
    QCoreApplication::sendEvent(&timeline, &event);
}

void scrubAcross(TimelineWidget& timeline, int moveCount)
{
    const qreal left = 16.0;
    const qreal right = std::max<qreal>(left + 1.0, timeline.width() - 16.0);
    sendMouse(
        timeline,
        QEvent::MouseButtonPress,
        timelinePoint(timeline, left),
        Qt::LeftButton,
        Qt::LeftButton);
    for (int move = 0; move < moveCount; ++move) {
        const qreal ratio = moveCount > 1
            ? static_cast<qreal>(move) / static_cast<qreal>(moveCount - 1)
            : 1.0;
        const qreal x = left + (right - left) * ratio;
        sendMouse(
            timeline,
            QEvent::MouseMove,
            timelinePoint(timeline, x),
            Qt::NoButton,
            Qt::LeftButton);
    }
    sendMouse(
        timeline,
        QEvent::MouseButtonRelease,
        timelinePoint(timeline, right),
        Qt::LeftButton,
        Qt::NoButton);
}

void clickAt(TimelineWidget& timeline, qreal x)
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

void panTimeline(TimelineWidget& timeline)
{
    const auto start = timelinePoint(
        timeline,
        static_cast<qreal>(timeline.width()) * 0.55);
    const auto end = timelinePoint(
        timeline,
        static_cast<qreal>(timeline.width()) * 0.35);
    sendMouse(
        timeline,
        QEvent::MouseButtonPress,
        start,
        Qt::MiddleButton,
        Qt::MiddleButton);
    sendMouse(
        timeline,
        QEvent::MouseMove,
        end,
        Qt::NoButton,
        Qt::MiddleButton);
    sendMouse(
        timeline,
        QEvent::MouseButtonRelease,
        end,
        Qt::MiddleButton,
        Qt::NoButton);
}

[[nodiscard]] bool validTimelineState(const TimelineWidget& timeline)
{
    const auto& model = timeline.model();
    return model.hasMedia()
        && model.viewportStart() >= vidscope::media::MediaTime::zero()
        && model.viewportStart() <= model.viewportEnd()
        && model.viewportEnd() <= model.duration()
        && model.playhead() >= vidscope::media::MediaTime::zero()
        && model.playhead() <= model.duration()
        && model.knownFrameCount() <= model.maximumKnownFrames();
}

} // namespace

VIDSCOPE_TEST(Phase3Timeline_real_VFR_rapid_interactions_shutdown_with_queued_work)
{
    auto window = std::make_unique<MainWindow>();
    window->resize(1'200, 800);
    window->show();
    QCoreApplication::processEvents(QEventLoop::AllEvents);

    const auto objects = findWindowObjects(*window);
    openVfrFixture(*window, *objects.controller, *objects.timeline, 1);

    // Let several real VFR frames reach the timeline before stress input.
    objects.controller->play();
    const bool accumulatedFrames = waitUntil(
        [&] { return objects.timeline->model().knownFrameCount() >= 3; },
        5'000);
    objects.controller->pause();
    VIDSCOPE_REQUIRE(accumulatedFrames);
    VIDSCOPE_REQUIRE(objects.timeline->model().knownFrameCount() >= 3);

    QSignalSpy seekSpy(objects.timeline, &TimelineWidget::seekRequested);
    QSignalSpy viewportSpy(objects.timeline, &TimelineWidget::viewportChanged);
    VIDSCOPE_REQUIRE(seekSpy.isValid());
    VIDSCOPE_REQUIRE(viewportSpy.isValid());

    QElapsedTimer interactionTimer;
    interactionTimer.start();
    const auto center = timelinePoint(
        *objects.timeline,
        static_cast<qreal>(objects.timeline->width()) / 2.0);
    for (int zoom = 0; zoom < 6; ++zoom) {
        sendWheel(
            *objects.timeline,
            center,
            QWheelEvent::DefaultDeltasPerStep);
    }
    panTimeline(*objects.timeline);
    scrubAcross(*objects.timeline, 128);

    for (int request = 0; request < 80; ++request) {
        const qreal usable = std::max<qreal>(1.0, objects.timeline->width() - 32.0);
        const qreal x = 16.0 + static_cast<qreal>((request * 53) % 997) / 996.0 * usable;
        clickAt(*objects.timeline, x);
        if (request % 8 == 0) {
            sendWheel(
                *objects.timeline,
                timelinePoint(*objects.timeline, x),
                request % 16 == 0
                    ? QWheelEvent::DefaultDeltasPerStep
                    : -QWheelEvent::DefaultDeltasPerStep);
        }
        if (request % 20 == 0) {
            panTimeline(*objects.timeline);
        }
    }

    VIDSCOPE_REQUIRE(interactionTimer.elapsed() < 5'000);
    VIDSCOPE_REQUIRE(seekSpy.count() >= 100);
    VIDSCOPE_REQUIRE(seekSpy.count() < 1'000);
    VIDSCOPE_REQUIRE(viewportSpy.count() >= 2);
    VIDSCOPE_REQUIRE(validTimelineState(*objects.timeline));

    // Queue more decoder work and destroy immediately without draining the GUI
    // queue first. MainWindow must synchronously join the controller worker.
    objects.controller->play();
    scrubAcross(*objects.timeline, 96);
    for (int request = 0; request < 32; ++request) {
        const qreal x = request % 2 == 0
            ? 24.0
            : static_cast<qreal>(objects.timeline->width()) - 24.0;
        clickAt(*objects.timeline, x);
    }

    QPointer<MainWindow> windowGuard(window.get());
    QPointer<PlaybackController> controllerGuard(objects.controller);
    QPointer<TimelineWidget> timelineGuard(objects.timeline);
    QElapsedTimer shutdownTimer;
    shutdownTimer.start();
    window->close();
    window.reset();
    VIDSCOPE_REQUIRE(shutdownTimer.elapsed() < 5'000);
    VIDSCOPE_REQUIRE(windowGuard.isNull());
    VIDSCOPE_REQUIRE(controllerGuard.isNull());
    VIDSCOPE_REQUIRE(timelineGuard.isNull());

    // Stale queued callbacks must be harmless after every receiver is gone.
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}

VIDSCOPE_TEST(Phase3Timeline_repeated_real_VFR_window_lifecycles_close_cleanly)
{
    constexpr int lifecycleCount = 3;
    QElapsedTimer totalTimer;
    totalTimer.start();

    for (int lifecycle = 0; lifecycle < lifecycleCount; ++lifecycle) {
        auto window = std::make_unique<MainWindow>();
        window->resize(1'000, 700);
        window->show();
        QCoreApplication::processEvents(QEventLoop::AllEvents);

        const auto objects = findWindowObjects(*window);
        openVfrFixture(*window, *objects.controller, *objects.timeline, 1);
        VIDSCOPE_REQUIRE(validTimelineState(*objects.timeline));

        const auto center = timelinePoint(
            *objects.timeline,
            static_cast<qreal>(objects.timeline->width()) / 2.0);
        sendWheel(
            *objects.timeline,
            center,
            QWheelEvent::DefaultDeltasPerStep);
        panTimeline(*objects.timeline);
        scrubAcross(*objects.timeline, 24);
        objects.controller->play();
        clickAt(
            *objects.timeline,
            lifecycle % 2 == 0
                ? 28.0
                : static_cast<qreal>(objects.timeline->width()) - 28.0);

        QPointer<MainWindow> guard(window.get());
        QElapsedTimer shutdownTimer;
        shutdownTimer.start();
        window->close();
        window.reset();
        VIDSCOPE_REQUIRE(shutdownTimer.elapsed() < 5'000);
        VIDSCOPE_REQUIRE(guard.isNull());
        QCoreApplication::processEvents(QEventLoop::AllEvents);
    }

    VIDSCOPE_REQUIRE(totalTimer.elapsed() < 45'000);
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
    QApplication::setApplicationName(QStringLiteral("Phase3TimelineStressTests"));
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
