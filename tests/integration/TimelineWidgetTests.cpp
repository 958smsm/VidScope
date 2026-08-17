#include "TestHarness.h"

#include "timeline/TimelineWidget.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QElapsedTimer>
#include <QtCore/QVariant>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QWheelEvent>
#include <QtTest/QSignalSpy>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>

using namespace std::chrono_literals;

namespace {

using vidscope::media::MediaTime;
using vidscope::timeline::TimelineMarkerKind;
using vidscope::timeline::TimelineWidget;

constexpr int kWidgetWidth = 1'000;
constexpr int kWidgetHeight = 160;

void showWidget(TimelineWidget& widget)
{
    widget.resize(kWidgetWidth, kWidgetHeight);
    widget.show();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}

[[nodiscard]] QPointF pointAt(const TimelineWidget& widget, qreal x)
{
    return {x, static_cast<qreal>(widget.height()) / 2.0};
}

void sendMouse(
    TimelineWidget& widget,
    QEvent::Type type,
    const QPointF& localPosition,
    Qt::MouseButton button,
    Qt::MouseButtons buttons,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const QPointF globalPosition(widget.mapToGlobal(localPosition.toPoint()));
    QMouseEvent event(
        type,
        localPosition,
        globalPosition,
        button,
        buttons,
        modifiers);
    QCoreApplication::sendEvent(&widget, &event);
}

void sendWheel(TimelineWidget& widget, const QPointF& localPosition, int angleDeltaY)
{
    const QPointF globalPosition(widget.mapToGlobal(localPosition.toPoint()));
    QWheelEvent event(
        localPosition,
        globalPosition,
        {},
        QPoint(0, angleDeltaY),
        Qt::NoButton,
        Qt::NoModifier,
        Qt::NoScrollPhase,
        false);
    QCoreApplication::sendEvent(&widget, &event);
}

[[nodiscard]] qint64 spyTime(const QSignalSpy& spy, int emission, int argument = 0)
{
    return spy.at(emission).at(argument).toLongLong();
}

[[nodiscard]] std::int64_t absoluteDifference(MediaTime left, MediaTime right)
{
    const auto difference = (left - right).count();
    return difference < 0 ? -difference : difference;
}

[[nodiscard]] bool validViewport(const TimelineWidget& widget)
{
    const auto& model = widget.model();
    return model.viewportStart() >= MediaTime::zero()
        && model.viewportStart() <= model.viewportEnd()
        && model.viewportEnd() <= model.duration();
}

void renderWidget(TimelineWidget& widget)
{
    QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    widget.render(&painter);
    painter.end();
    VIDSCOPE_REQUIRE(!image.isNull());
}

} // namespace

VIDSCOPE_TEST(TimelineWidget_click_and_drag_emit_clamped_seek_and_scrub_state)
{
    TimelineWidget widget;
    showWidget(widget);
    widget.setDuration(MediaTime{10s}.count());

    QSignalSpy seekSpy(&widget, &TimelineWidget::seekRequested);
    QSignalSpy scrubSpy(&widget, &TimelineWidget::scrubbingChanged);
    VIDSCOPE_REQUIRE(seekSpy.isValid());
    VIDSCOPE_REQUIRE(scrubSpy.isValid());

    const auto center = pointAt(widget, static_cast<qreal>(widget.width()) / 2.0);
    sendMouse(widget, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);

    VIDSCOPE_REQUIRE(seekSpy.count() == 1);
    VIDSCOPE_REQUIRE(std::llabs(spyTime(seekSpy, 0) - MediaTime{5s}.count()) <= 1);
    VIDSCOPE_REQUIRE(scrubSpy.count() == 2);
    VIDSCOPE_REQUIRE(scrubSpy.at(0).at(0).toBool());
    VIDSCOPE_REQUIRE(!scrubSpy.at(1).at(0).toBool());

    seekSpy.clear();
    scrubSpy.clear();
    const auto left = pointAt(widget, 12.0);
    const auto right = pointAt(widget, static_cast<qreal>(widget.width()) + 100.0);
    sendMouse(widget, QEvent::MouseButtonPress, left, Qt::LeftButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseMove, right, Qt::NoButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseButtonRelease, right, Qt::LeftButton, Qt::NoButton);

    VIDSCOPE_REQUIRE(seekSpy.count() == 2);
    VIDSCOPE_REQUIRE(spyTime(seekSpy, 0) == 0);
    VIDSCOPE_REQUIRE(spyTime(seekSpy, seekSpy.count() - 1) == MediaTime{10s}.count());
    VIDSCOPE_REQUIRE(scrubSpy.count() == 2);
}

VIDSCOPE_TEST(TimelineWidget_ignores_external_position_updates_during_scrub)
{
    TimelineWidget widget;
    showWidget(widget);
    widget.setDuration(MediaTime{10s}.count());

    const auto pressPoint = pointAt(widget, 250.0);
    sendMouse(widget, QEvent::MouseButtonPress, pressPoint, Qt::LeftButton, Qt::LeftButton);
    const auto scrubPosition = widget.model().playhead();
    VIDSCOPE_REQUIRE(scrubPosition < 5s);

    widget.setPosition(MediaTime{9s}.count());
    VIDSCOPE_REQUIRE(widget.model().playhead() == scrubPosition);

    sendMouse(widget, QEvent::MouseButtonRelease, pressPoint, Qt::LeftButton, Qt::NoButton);
    widget.setPosition(MediaTime{9s}.count());
    VIDSCOPE_REQUIRE(widget.model().playhead() == 9s);
}

VIDSCOPE_TEST(TimelineWidget_wheel_zoom_preserves_the_cursor_anchor)
{
    TimelineWidget widget;
    showWidget(widget);
    widget.setDuration(MediaTime{100s}.count());

    QSignalSpy viewportSpy(&widget, &TimelineWidget::viewportChanged);
    VIDSCOPE_REQUIRE(viewportSpy.isValid());

    const auto center = pointAt(widget, static_cast<qreal>(widget.width()) / 2.0);
    const auto beforeSpan = widget.model().visibleDuration();
    const auto beforeCenter = widget.model().viewportStart() + beforeSpan / 2;
    sendWheel(widget, center, QWheelEvent::DefaultDeltasPerStep);

    const auto afterSpan = widget.model().visibleDuration();
    const auto afterCenter = widget.model().viewportStart() + afterSpan / 2;
    VIDSCOPE_REQUIRE(afterSpan < beforeSpan);
    VIDSCOPE_REQUIRE(absoluteDifference(beforeCenter, afterCenter) <= 1);
    VIDSCOPE_REQUIRE(viewportSpy.count() == 1);
    VIDSCOPE_REQUIRE(validViewport(widget));
}

VIDSCOPE_TEST(TimelineWidget_middle_drag_pans_without_requesting_a_seek)
{
    TimelineWidget widget;
    showWidget(widget);
    widget.setDuration(MediaTime{100s}.count());
    widget.setPosition(MediaTime{50s}.count());
    widget.zoomIn();
    widget.zoomIn();

    QSignalSpy seekSpy(&widget, &TimelineWidget::seekRequested);
    QSignalSpy viewportSpy(&widget, &TimelineWidget::viewportChanged);
    VIDSCOPE_REQUIRE(seekSpy.isValid());
    VIDSCOPE_REQUIRE(viewportSpy.isValid());
    seekSpy.clear();
    viewportSpy.clear();

    const auto oldStart = widget.model().viewportStart();
    const auto oldSpan = widget.model().visibleDuration();
    const auto center = pointAt(widget, 500.0);
    const auto moved = pointAt(widget, 700.0);
    sendMouse(widget, QEvent::MouseButtonPress, center, Qt::MiddleButton, Qt::MiddleButton);
    sendMouse(widget, QEvent::MouseMove, moved, Qt::NoButton, Qt::MiddleButton);
    sendMouse(widget, QEvent::MouseButtonRelease, moved, Qt::MiddleButton, Qt::NoButton);

    VIDSCOPE_REQUIRE(widget.model().viewportStart() != oldStart);
    VIDSCOPE_REQUIRE(absoluteDifference(widget.model().visibleDuration(), oldSpan) <= 1);
    VIDSCOPE_REQUIRE(viewportSpy.count() >= 1);
    VIDSCOPE_REQUIRE(seekSpy.isEmpty());
    VIDSCOPE_REQUIRE(validViewport(widget));
}

VIDSCOPE_TEST(TimelineWidget_forward_and_reverse_selection_are_normalized)
{
    TimelineWidget widget;
    showWidget(widget);
    widget.setDuration(MediaTime{10s}.count());

    QSignalSpy selectionSpy(&widget, &TimelineWidget::selectionChanged);
    VIDSCOPE_REQUIRE(selectionSpy.isValid());

    const auto quarter = pointAt(widget, 250.0);
    const auto threeQuarter = pointAt(widget, 750.0);
    sendMouse(
        widget,
        QEvent::MouseButtonPress,
        quarter,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::ShiftModifier);
    sendMouse(
        widget,
        QEvent::MouseMove,
        threeQuarter,
        Qt::NoButton,
        Qt::LeftButton,
        Qt::ShiftModifier);
    sendMouse(
        widget,
        QEvent::MouseButtonRelease,
        threeQuarter,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::ShiftModifier);

    VIDSCOPE_REQUIRE(widget.model().selection().has_value());
    const auto forward = *widget.model().selection();
    VIDSCOPE_REQUIRE(forward.start < forward.end);
    VIDSCOPE_REQUIRE(selectionSpy.count() >= 1);
    VIDSCOPE_REQUIRE(selectionSpy.at(selectionSpy.count() - 1).at(2).toBool());

    widget.clearSelection();
    sendMouse(
        widget,
        QEvent::MouseButtonPress,
        threeQuarter,
        Qt::LeftButton,
        Qt::LeftButton,
        Qt::ShiftModifier);
    sendMouse(
        widget,
        QEvent::MouseMove,
        quarter,
        Qt::NoButton,
        Qt::LeftButton,
        Qt::ShiftModifier);
    sendMouse(
        widget,
        QEvent::MouseButtonRelease,
        quarter,
        Qt::LeftButton,
        Qt::NoButton,
        Qt::ShiftModifier);

    VIDSCOPE_REQUIRE(widget.model().selection().has_value());
    const auto reverse = *widget.model().selection();
    VIDSCOPE_REQUIRE(reverse.start < reverse.end);
    VIDSCOPE_REQUIRE(absoluteDifference(forward.start, reverse.start) <= 1);
    VIDSCOPE_REQUIRE(absoluteDifference(forward.end, reverse.end) <= 1);
}

VIDSCOPE_TEST(TimelineWidget_marker_hit_activates_and_seeks_to_the_exact_marker_time)
{
    TimelineWidget widget;
    showWidget(widget);
    widget.setDuration(MediaTime{10s}.count());
    const auto markerId = widget.addMarker(
        MediaTime{5s}.count(),
        TimelineMarkerKind::Bookmark,
        QStringLiteral("inspection"));
    VIDSCOPE_REQUIRE(markerId.has_value());

    QSignalSpy markerSpy(&widget, &TimelineWidget::markerActivated);
    QSignalSpy seekSpy(&widget, &TimelineWidget::seekRequested);
    VIDSCOPE_REQUIRE(markerSpy.isValid());
    VIDSCOPE_REQUIRE(seekSpy.isValid());

    const auto center = pointAt(widget, static_cast<qreal>(widget.width()) / 2.0);
    sendMouse(widget, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    sendMouse(widget, QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);

    VIDSCOPE_REQUIRE(markerSpy.count() == 1);
    VIDSCOPE_REQUIRE(markerSpy.at(0).at(0).toULongLong() == *markerId);
    VIDSCOPE_REQUIRE(spyTime(markerSpy, 0, 1) == MediaTime{5s}.count());
    VIDSCOPE_REQUIRE(seekSpy.count() == 1);
    VIDSCOPE_REQUIRE(spyTime(seekSpy, 0) == MediaTime{5s}.count());
}

VIDSCOPE_TEST(TimelineWidget_paints_zero_and_multi_hour_ranges)
{
    TimelineWidget widget;
    showWidget(widget);
    renderWidget(widget);

    constexpr auto longDuration = 12h;
    widget.setDuration(MediaTime{longDuration}.count());
    widget.setPosition(MediaTime{6h}.count());
    VIDSCOPE_REQUIRE(widget.addMarker(
        0,
        TimelineMarkerKind::Chapter,
        QStringLiteral("start")).has_value());
    VIDSCOPE_REQUIRE(widget.addMarker(
        MediaTime{6h}.count(),
        TimelineMarkerKind::Bookmark,
        QStringLiteral("middle")).has_value());
    VIDSCOPE_REQUIRE(widget.addMarker(
        MediaTime{longDuration}.count(),
        TimelineMarkerKind::Scene,
        QStringLiteral("end")).has_value());
    widget.setInPointAtPlayhead();
    widget.setPosition(MediaTime{9h}.count());
    widget.setOutPointAtPlayhead();

    vidscope::media::DecodedFrame frame;
    frame.id = {.presentationIndex = 42, .pts = 90'000, .sessionSerial = 1};
    frame.presentationTime = 6h;
    frame.duration = 41'708'333ns;
    frame.keyFrame = true;
    widget.observeFrame(frame);

    renderWidget(widget);
    VIDSCOPE_REQUIRE(widget.model().duration() == longDuration);
    VIDSCOPE_REQUIRE(widget.model().knownFrameCount() == 1);
    VIDSCOPE_REQUIRE(widget.model().markers().size() == 3);
    VIDSCOPE_REQUIRE(widget.model().selection().has_value());
}

VIDSCOPE_TEST(TimelineWidget_zero_duration_resets_all_media_state)
{
    TimelineWidget widget;
    showWidget(widget);
    widget.setDuration(MediaTime{30s}.count());
    widget.setPosition(MediaTime{12s}.count());
    VIDSCOPE_REQUIRE(widget.addMarker(
        MediaTime{10s}.count(),
        TimelineMarkerKind::Bookmark).has_value());
    widget.setInPointAtPlayhead();
    widget.setPosition(MediaTime{20s}.count());
    widget.setOutPointAtPlayhead();
    widget.zoomIn();

    vidscope::media::DecodedFrame frame;
    frame.id = {.presentationIndex = 1, .pts = 1, .sessionSerial = 1};
    frame.presentationTime = 1s;
    frame.duration = 40ms;
    widget.observeFrame(frame);

    widget.setDuration(0);
    const auto& model = widget.model();
    VIDSCOPE_REQUIRE(!model.hasMedia());
    VIDSCOPE_REQUIRE(model.duration() == MediaTime::zero());
    VIDSCOPE_REQUIRE(model.viewportStart() == MediaTime::zero());
    VIDSCOPE_REQUIRE(model.viewportEnd() == MediaTime::zero());
    VIDSCOPE_REQUIRE(model.playhead() == MediaTime::zero());
    VIDSCOPE_REQUIRE(model.knownFrames().empty());
    VIDSCOPE_REQUIRE(model.markers().empty());
    VIDSCOPE_REQUIRE(!model.selection().has_value());
}

VIDSCOPE_TEST(TimelineWidget_rapid_repeated_interactions_remain_bounded_and_valid)
{
    TimelineWidget widget;
    showWidget(widget);
    widget.setDuration(MediaTime{6h}.count());
    for (int zoom = 0; zoom < 8; ++zoom) {
        widget.zoomIn();
    }

    QSignalSpy seekSpy(&widget, &TimelineWidget::seekRequested);
    VIDSCOPE_REQUIRE(seekSpy.isValid());

    const auto center = pointAt(widget, 500.0);
    const auto repeated = pointAt(widget, 700.0);
    sendMouse(widget, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    for (int request = 0; request < 2'000; ++request) {
        sendMouse(widget, QEvent::MouseMove, repeated, Qt::NoButton, Qt::LeftButton);
    }
    sendMouse(widget, QEvent::MouseButtonRelease, repeated, Qt::LeftButton, Qt::NoButton);

    // The press and one changed drag position are the only useful requests;
    // identical moves and an unchanged release must not grow delivery.
    VIDSCOPE_REQUIRE(seekSpy.count() == 2);

    QElapsedTimer interactionTimer;
    interactionTimer.start();
    for (int interaction = 0; interaction < 256; ++interaction) {
        const qreal x = 100.0 + static_cast<qreal>((interaction * 37) % 800);
        sendWheel(
            widget,
            pointAt(widget, x),
            interaction % 2 == 0
                ? QWheelEvent::DefaultDeltasPerStep
                : -QWheelEvent::DefaultDeltasPerStep);
    }
    VIDSCOPE_REQUIRE(interactionTimer.elapsed() < 5'000);
    VIDSCOPE_REQUIRE(validViewport(widget));
    VIDSCOPE_REQUIRE(widget.model().visibleDuration() > MediaTime::zero());
    VIDSCOPE_REQUIRE(widget.model().knownFrameCount()
                     <= widget.model().maximumKnownFrames());
}

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    }
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    return vidscope::test::runAll();
}
