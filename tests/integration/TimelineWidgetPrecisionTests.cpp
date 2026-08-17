#include "TestHarness.h"

#include "timeline/TimelineWidget.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtGui/QImage>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QWheelEvent>
#include <QtTest/QSignalSpy>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>

using namespace std::chrono_literals;

namespace {

using vidscope::media::MediaTime;
using vidscope::timeline::TimelineMarkerKind;
using vidscope::timeline::TimelineModel;
using vidscope::timeline::TimelineWidget;

constexpr int kWidgetWidth = 1'000;
constexpr int kWidgetHeight = 154;
constexpr qreal kTrackInset = 12.0;
constexpr QColor kSentinelColor(255, 0, 255);

void showWidget(TimelineWidget& widget)
{
    widget.resize(kWidgetWidth, kWidgetHeight);
    widget.show();
    QCoreApplication::processEvents(QEventLoop::AllEvents);
}

[[nodiscard]] qreal trackWidth(const TimelineWidget& widget)
{
    return std::max<qreal>(
        1.0,
        static_cast<qreal>(widget.width()) - 2.0 * kTrackInset);
}

[[nodiscard]] qreal xAtFraction(const TimelineWidget& widget, qreal fraction)
{
    return kTrackInset + trackWidth(widget) * fraction;
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
    Qt::MouseButtons buttons)
{
    const QPointF globalPosition(widget.mapToGlobal(localPosition.toPoint()));
    QMouseEvent event(
        type,
        localPosition,
        globalPosition,
        button,
        buttons,
        Qt::NoModifier);
    QCoreApplication::sendEvent(&widget, &event);
}

void sendWheel(
    TimelineWidget& widget,
    const QPointF& localPosition,
    int angleDeltaY,
    Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    const QPointF globalPosition(widget.mapToGlobal(localPosition.toPoint()));
    QWheelEvent event(
        localPosition,
        globalPosition,
        {},
        QPoint(0, angleDeltaY),
        Qt::NoButton,
        modifiers,
        Qt::NoScrollPhase,
        false);
    QCoreApplication::sendEvent(&widget, &event);
}

void middlePan(TimelineWidget& widget, qreal pressX, qreal releaseX)
{
    sendMouse(
        widget,
        QEvent::MouseButtonPress,
        pointAt(widget, pressX),
        Qt::MiddleButton,
        Qt::MiddleButton);
    sendMouse(
        widget,
        QEvent::MouseMove,
        pointAt(widget, releaseX),
        Qt::NoButton,
        Qt::MiddleButton);
    sendMouse(
        widget,
        QEvent::MouseButtonRelease,
        pointAt(widget, releaseX),
        Qt::MiddleButton,
        Qt::NoButton);
}

void click(TimelineWidget& widget, qreal x)
{
    const auto point = pointAt(widget, x);
    sendMouse(
        widget,
        QEvent::MouseButtonPress,
        point,
        Qt::LeftButton,
        Qt::LeftButton);
    sendMouse(
        widget,
        QEvent::MouseButtonRelease,
        point,
        Qt::LeftButton,
        Qt::NoButton);
}

[[nodiscard]] std::int64_t absoluteDifference(MediaTime left, MediaTime right)
{
    const auto difference = (left - right).count();
    return difference < 0 ? -difference : difference;
}

void requireViewportEquals(const TimelineModel& actual, const TimelineModel& expected)
{
    VIDSCOPE_REQUIRE(actual.viewportStart() == expected.viewportStart());
    VIDSCOPE_REQUIRE(actual.viewportEnd() == expected.viewportEnd());
}

[[nodiscard]] QImage renderToSentinelImage(TimelineWidget& widget)
{
    QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(kSentinelColor);
    QPainter painter(&image);
    widget.render(&painter);
    painter.end();
    return image;
}

[[nodiscard]] std::size_t changedPixelCount(
    const QImage& before,
    const QImage& after,
    int left,
    int right,
    int top,
    int bottom)
{
    VIDSCOPE_REQUIRE(before.size() == after.size());
    left = std::clamp(left, 0, before.width());
    right = std::clamp(right, 0, before.width());
    top = std::clamp(top, 0, before.height());
    bottom = std::clamp(bottom, 0, before.height());

    std::size_t changed = 0;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            if (before.pixel(x, y) != after.pixel(x, y)) {
                ++changed;
            }
        }
    }
    return changed;
}

} // namespace

VIDSCOPE_TEST(TimelineWidget_off_center_wheel_zoom_preserves_25_and_75_percent_anchors)
{
    TimelineWidget widget;
    showWidget(widget);
    widget.setDuration(MediaTime{100s}.count());

    for (const qreal fraction : {0.25, 0.75}) {
        widget.showEntireMedia();
        const qreal x = xAtFraction(widget, fraction);
        const auto before = widget.model().pixelToTime(
            x,
            kTrackInset,
            trackWidth(widget));
        sendWheel(
            widget,
            pointAt(widget, x),
            QWheelEvent::DefaultDeltasPerStep);
        const auto after = widget.model().pixelToTime(
            x,
            kTrackInset,
            trackWidth(widget));

        VIDSCOPE_REQUIRE(widget.model().visibleDuration() < 100s);
        VIDSCOPE_REQUIRE(absoluteDifference(before, after) <= 1);
    }
}

VIDSCOPE_TEST(TimelineWidget_middle_pan_matches_exact_model_mapping_in_both_directions_and_edges)
{
    TimelineWidget widget;
    showWidget(widget);
    widget.setDuration(MediaTime{100s}.count());
    widget.setPosition(MediaTime{50s}.count());
    widget.zoomIn();
    widget.zoomIn();

    const qreal pressX = xAtFraction(widget, 0.5);
    const qreal leftRelease = pressX - 100.0;
    TimelineModel expectedLater = widget.model();
    VIDSCOPE_REQUIRE(expectedLater.panByPixels(100.0, trackWidth(widget)));
    middlePan(widget, pressX, leftRelease);
    requireViewportEquals(widget.model(), expectedLater);

    const qreal rightRelease = pressX + 100.0;
    TimelineModel expectedEarlier = widget.model();
    VIDSCOPE_REQUIRE(expectedEarlier.panByPixels(-100.0, trackWidth(widget)));
    middlePan(widget, pressX, rightRelease);
    requireViewportEquals(widget.model(), expectedEarlier);

    TimelineModel expectedEnd = widget.model();
    const qreal farLeft = -100'000.0;
    VIDSCOPE_REQUIRE(expectedEnd.panByPixels(
        pressX - farLeft,
        trackWidth(widget)));
    middlePan(widget, pressX, farLeft);
    requireViewportEquals(widget.model(), expectedEnd);
    VIDSCOPE_REQUIRE(widget.model().viewportEnd() == widget.model().duration());

    TimelineModel expectedStart = widget.model();
    const qreal farRight = 100'000.0;
    VIDSCOPE_REQUIRE(expectedStart.panByPixels(
        pressX - farRight,
        trackWidth(widget)));
    middlePan(widget, pressX, farRight);
    requireViewportEquals(widget.model(), expectedStart);
    VIDSCOPE_REQUIRE(widget.model().viewportStart() == MediaTime::zero());
}

VIDSCOPE_TEST(TimelineWidget_shift_wheel_pans_horizontally_without_seeking)
{
    TimelineWidget widget;
    showWidget(widget);
    widget.setDuration(MediaTime{100s}.count());
    widget.setPosition(MediaTime{50s}.count());
    widget.zoomIn();
    widget.zoomIn();

    QSignalSpy seekSpy(&widget, &TimelineWidget::seekRequested);
    VIDSCOPE_REQUIRE(seekSpy.isValid());

    const qreal x = xAtFraction(widget, 0.5);
    const double wheelPixels = static_cast<double>(trackWidth(widget)) * 0.1;
    TimelineModel expectedEarlier = widget.model();
    VIDSCOPE_REQUIRE(expectedEarlier.panByPixels(
        -wheelPixels,
        trackWidth(widget)));
    sendWheel(
        widget,
        pointAt(widget, x),
        QWheelEvent::DefaultDeltasPerStep,
        Qt::ShiftModifier);
    requireViewportEquals(widget.model(), expectedEarlier);

    TimelineModel expectedLater = widget.model();
    VIDSCOPE_REQUIRE(expectedLater.panByPixels(
        wheelPixels,
        trackWidth(widget)));
    sendWheel(
        widget,
        pointAt(widget, x),
        -QWheelEvent::DefaultDeltasPerStep,
        Qt::ShiftModifier);
    requireViewportEquals(widget.model(), expectedLater);
    VIDSCOPE_REQUIRE(seekSpy.isEmpty());
}

VIDSCOPE_TEST(TimelineWidget_marker_hit_selects_nearest_and_outside_radius_uses_raw_time)
{
    TimelineWidget widget;
    showWidget(widget);
    widget.setDuration(MediaTime{10s}.count());

    const auto marker20 = widget.addMarker(
        MediaTime{2s}.count(), TimelineMarkerKind::Bookmark, QStringLiteral("twenty"));
    const auto marker50 = widget.addMarker(
        MediaTime{5s}.count(), TimelineMarkerKind::Scene, QStringLiteral("fifty"));
    const auto duplicate50 = widget.addMarker(
        MediaTime{5s}.count(), TimelineMarkerKind::Chapter, QStringLiteral("duplicate"));
    const auto marker80 = widget.addMarker(
        MediaTime{8s}.count(), TimelineMarkerKind::Bookmark, QStringLiteral("eighty"));
    VIDSCOPE_REQUIRE(marker20 && marker50 && duplicate50 && marker80);

    QSignalSpy markerSpy(&widget, &TimelineWidget::markerActivated);
    QSignalSpy seekSpy(&widget, &TimelineWidget::seekRequested);
    VIDSCOPE_REQUIRE(markerSpy.isValid());
    VIDSCOPE_REQUIRE(seekSpy.isValid());

    const qreal x50 = widget.model().timeToPixel(
        5s,
        kTrackInset,
        trackWidth(widget));
    click(widget, x50 + 6.0);
    VIDSCOPE_REQUIRE(markerSpy.count() == 1);
    VIDSCOPE_REQUIRE(markerSpy.at(0).at(0).toULongLong() == *duplicate50);
    VIDSCOPE_REQUIRE(markerSpy.at(0).at(1).toLongLong() == MediaTime{5s}.count());
    VIDSCOPE_REQUIRE(seekSpy.count() == 1);
    VIDSCOPE_REQUIRE(seekSpy.at(seekSpy.count() - 1).at(0).toLongLong()
                     == MediaTime{5s}.count());

    markerSpy.clear();
    seekSpy.clear();
    const qreal outsideX = x50 + 10.0;
    const auto rawTime = widget.model().pixelToTime(
        outsideX,
        kTrackInset,
        trackWidth(widget));
    click(widget, outsideX);
    VIDSCOPE_REQUIRE(markerSpy.isEmpty());
    VIDSCOPE_REQUIRE(seekSpy.count() == 1);
    VIDSCOPE_REQUIRE(seekSpy.at(seekSpy.count() - 1).at(0).toLongLong()
                     == rawTime.count());
    VIDSCOPE_REQUIRE(rawTime != 5s);

    markerSpy.clear();
    seekSpy.clear();
    const qreal x80 = widget.model().timeToPixel(
        8s,
        kTrackInset,
        trackWidth(widget));
    click(widget, x80 - 5.0);
    VIDSCOPE_REQUIRE(markerSpy.count() == 1);
    VIDSCOPE_REQUIRE(markerSpy.at(0).at(0).toULongLong() == *marker80);
    VIDSCOPE_REQUIRE(seekSpy.count() == 1);

    markerSpy.clear();
    seekSpy.clear();
    const qreal betweenX = widget.model().timeToPixel(
        3'500ms,
        kTrackInset,
        trackWidth(widget));
    click(widget, betweenX);
    VIDSCOPE_REQUIRE(markerSpy.isEmpty());
    VIDSCOPE_REQUIRE(seekSpy.count() == 1);
}

VIDSCOPE_TEST(TimelineWidget_render_changes_sentinel_and_draws_marker_and_selection_layers)
{
    TimelineWidget widget;
    showWidget(widget);
    widget.setDuration(MediaTime{10s}.count());

    const auto baseline = renderToSentinelImage(widget);
    std::size_t sentinelPixels = 0;
    for (int y = 0; y < baseline.height(); ++y) {
        for (int x = 0; x < baseline.width(); ++x) {
            if (baseline.pixelColor(x, y) == kSentinelColor) {
                ++sentinelPixels;
            }
        }
    }
    const auto totalPixels = static_cast<std::size_t>(
        baseline.width()) * static_cast<std::size_t>(baseline.height());
    VIDSCOPE_REQUIRE(sentinelPixels < totalPixels / 10);

    VIDSCOPE_REQUIRE(widget.addMarker(
        MediaTime{5s}.count(),
        TimelineMarkerKind::Bookmark,
        QStringLiteral("paint evidence")).has_value());
    const auto withMarker = renderToSentinelImage(widget);
    const int markerX = static_cast<int>(std::lround(widget.model().timeToPixel(
        5s,
        kTrackInset,
        trackWidth(widget))));
    VIDSCOPE_REQUIRE(changedPixelCount(
        baseline,
        withMarker,
        markerX - 7,
        markerX + 8,
        20,
        widget.height() - 20)
        > 0);

    widget.setPosition(MediaTime{2s}.count());
    widget.setInPointAtPlayhead();
    widget.setPosition(MediaTime{8s}.count());
    widget.setOutPointAtPlayhead();
    const auto withSelection = renderToSentinelImage(widget);
    const int selectionInteriorX = static_cast<int>(std::lround(
        widget.model().timeToPixel(
            3'500ms,
            kTrackInset,
            trackWidth(widget))));
    VIDSCOPE_REQUIRE(changedPixelCount(
        withMarker,
        withSelection,
        selectionInteriorX - 2,
        selectionInteriorX + 3,
        29,
        widget.height() - 34)
        > 0);
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
