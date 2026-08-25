#include "timeline/TimelineWidget.h"

#include "analysis/AnalysisManager.h"

#include <QtCore/QEvent>
#include <QtGui/QFontMetricsF>
#include <QtGui/QFocusEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QSizePolicy>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>

namespace vidscope::timeline {
namespace {

using namespace std::chrono_literals;

constexpr qreal kHorizontalInset = 12.0;
constexpr qreal kRulerHeight = 29.0;
constexpr qreal kBottomAreaHeight = 34.0;
constexpr qreal kMarkerHitRadius = 8.0;
constexpr qreal kSelectionHandleRadius = 7.0;
constexpr qreal kOverviewHeight = 9.0;
constexpr qreal kOverviewBottomInset = 6.0;
constexpr double kZoomStep = 1.5;
constexpr std::size_t kMaximumFramePrimitives = 4'096;

[[nodiscard]] qint64 toNanoseconds(media::MediaTime time) noexcept
{
    return static_cast<qint64>(time.count());
}

[[nodiscard]] media::MediaTime nonNegativeTime(qint64 nanoseconds) noexcept
{
    return media::MediaTime(std::max<qint64>(0, nanoseconds));
}

[[nodiscard]] std::uint64_t absoluteDistance(
    media::MediaTime left,
    media::MediaTime right) noexcept
{
    const auto leftCount = left.count();
    const auto rightCount = right.count();
    return leftCount >= rightCount
        ? static_cast<std::uint64_t>(leftCount - rightCount)
        : static_cast<std::uint64_t>(rightCount - leftCount);
}

[[nodiscard]] QString formatTimelineTime(
    media::MediaTime time,
    media::MediaTime interval)
{
    constexpr std::int64_t nanosecondsPerSecond = 1'000'000'000;
    constexpr std::int64_t nanosecondsPerMillisecond = 1'000'000;
    constexpr std::int64_t nanosecondsPerMicrosecond = 1'000;

    const auto totalNanoseconds = std::max<std::int64_t>(0, time.count());
    const auto totalSeconds = totalNanoseconds / nanosecondsPerSecond;
    const auto seconds = totalSeconds % 60;
    const auto totalMinutes = totalSeconds / 60;
    const auto minutes = totalMinutes % 60;
    const auto hours = totalMinutes / 60;

    QString result;
    if (hours > 0) {
        result = QStringLiteral("%1:%2:%3")
                     .arg(hours)
                     .arg(minutes, 2, 10, QLatin1Char('0'))
                     .arg(seconds, 2, 10, QLatin1Char('0'));
    } else {
        result = QStringLiteral("%1:%2")
                     .arg(totalMinutes, 2, 10, QLatin1Char('0'))
                     .arg(seconds, 2, 10, QLatin1Char('0'));
    }

    const auto fractional = totalNanoseconds % nanosecondsPerSecond;
    if (interval < 1us) {
        result += QStringLiteral(".%1").arg(fractional, 9, 10, QLatin1Char('0'));
    } else if (interval < 1ms) {
        result += QStringLiteral(".%1")
                      .arg(fractional / nanosecondsPerMicrosecond, 6, 10, QLatin1Char('0'));
    } else if (interval < 1s) {
        result += QStringLiteral(".%1")
                      .arg(fractional / nanosecondsPerMillisecond, 3, 10, QLatin1Char('0'));
    }
    return result;
}

[[nodiscard]] QColor markerColor(TimelineMarkerKind kind) noexcept
{
    switch (kind) {
    case TimelineMarkerKind::Keyframe:
        return QColor(242, 184, 79);
    case TimelineMarkerKind::Scene:
        return QColor(218, 111, 255);
    case TimelineMarkerKind::Chapter:
        return QColor(72, 198, 224);
    case TimelineMarkerKind::Bookmark:
        return QColor(92, 214, 149);
    }
    return QColor(210, 216, 226);
}

[[nodiscard]] QPointF trianglePoint(qreal x, qreal y) noexcept
{
    return QPointF(x, y);
}

[[nodiscard]] media::MediaTime viewportAnchor(const TimelineModel& model) noexcept
{
    const auto playhead = model.playhead();
    if (playhead >= model.viewportStart() && playhead <= model.viewportEnd()) {
        return playhead;
    }
    return model.viewportStart() + model.visibleDuration() / 2;
}

[[nodiscard]] QString heatmapModeName(const HeatmapMode mode)
{
    switch (mode) {
    case HeatmapMode::Motion:
        return TimelineWidget::tr("Motion");
    case HeatmapMode::Similarity:
        return TimelineWidget::tr("Similarity");
    case HeatmapMode::SceneChange:
        return TimelineWidget::tr("Scene Change");
    case HeatmapMode::Combined:
        return TimelineWidget::tr("Combined");
    }
    return {};
}

} // namespace

TimelineWidget::TimelineWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("timelineWidget"));
    setAccessibleName(tr("Video timeline"));
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(112);
    setMaximumHeight(154);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setEnabled(false);
    updateCursorForMode();
}

void TimelineWidget::setDuration(qint64 nanoseconds)
{
    const auto duration = nonNegativeTime(nanoseconds);
    if (duration == model_.duration()) {
        setEnabled(model_.hasMedia());
        return;
    }

    cancelInteraction();

    if (hoverActive_) {
        hoverActive_ = false;
        emit hoverChanged(hoveredTime_, -1, false);
        emit hoverPreviewChanged(hoveredTime_, -1, QPoint{}, false);
    }

    model_.reset(duration);
    setEnabled(model_.hasMedia());
    emitViewportChanged();
    emitSelectionChanged();
    updateCursorForMode();
    update();
}

void TimelineWidget::setPosition(qint64 nanoseconds)
{
    if (interaction_ == InteractionMode::Scrub) {
        return;
    }
    if (model_.setPlayhead(nonNegativeTime(nanoseconds))) {
        update();
    }
}

void TimelineWidget::observeFrame(const media::DecodedFrame& frame)
{
    if (model_.observeFrame(frame)) {
        update();
    }
}

void TimelineWidget::setAnalysisManager(analysis::AnalysisManager* manager)
{
    if (analysisManager_ == manager) {
        return;
    }
    if (analysisManager_) {
        disconnect(analysisManager_, nullptr, this, nullptr);
    }
    analysisManager_ = manager;
    if (analysisManager_) {
        connect(
            analysisManager_,
            &analysis::AnalysisManager::samplesAvailable,
            this,
            [this](qint64, qint64, quint64) { update(); });
        connect(
            analysisManager_,
            &analysis::AnalysisManager::stateChanged,
            this,
            [this](analysis::AnalysisState) { update(); });
    }
    update();
}

void TimelineWidget::setHeatmapMode(const HeatmapMode mode)
{
    switch (mode) {
    case HeatmapMode::Motion:
    case HeatmapMode::Similarity:
    case HeatmapMode::SceneChange:
    case HeatmapMode::Combined:
        break;
    }
    if (heatmapMode_ == mode) {
        return;
    }
    heatmapMode_ = mode;
    setToolTip(tr("%1 analysis heatmap").arg(heatmapModeName(mode)));
    update();
}

void TimelineWidget::setCombinedHeatmapWeights(CombinedHeatmapWeights weights)
{
    weights.motion = std::max(0.0F, weights.motion);
    weights.similarityDifference = std::max(0.0F, weights.similarityDifference);
    weights.sceneChange = std::max(0.0F, weights.sceneChange);
    if (combinedHeatmapWeights_ == weights) {
        return;
    }
    combinedHeatmapWeights_ = weights;
    update();
}

HeatmapMode TimelineWidget::heatmapMode() const noexcept
{
    return heatmapMode_;
}

CombinedHeatmapWeights TimelineWidget::combinedHeatmapWeights() const noexcept
{
    return combinedHeatmapWeights_;
}

const TimelineModel& TimelineWidget::model() const noexcept
{
    return model_;
}

std::optional<std::uint64_t> TimelineWidget::addMarker(
    qint64 nanoseconds,
    TimelineMarkerKind kind,
    QString label)
{
    auto id = model_.addMarker(nonNegativeTime(nanoseconds), kind, std::move(label));
    if (id) {
        update();
    }
    return id;
}

bool TimelineWidget::removeMarker(std::uint64_t id)
{
    if (!model_.removeMarker(id)) {
        return false;
    }
    update();
    return true;
}

void TimelineWidget::clearMarkers(std::optional<TimelineMarkerKind> kind)
{
    model_.clearMarkers(kind);
    update();
}

std::optional<qint64> TimelineWidget::adjacentMarkerNanoseconds(
    TimelineMarkerKind kind,
    bool forward) const noexcept
{
    const auto time = model_.adjacentMarkerTime(kind, model_.playhead(), forward);
    if (!time) {
        return std::nullopt;
    }
    return toNanoseconds(*time);
}

void TimelineWidget::zoomIn()
{
    if (!model_.hasMedia()) {
        return;
    }
    if (model_.zoomAt(kZoomStep, viewportAnchor(model_))) {
        emitViewportChanged();
        update();
    }
}

void TimelineWidget::zoomOut()
{
    if (!model_.hasMedia()) {
        return;
    }
    if (model_.zoomAt(1.0 / kZoomStep, viewportAnchor(model_))) {
        emitViewportChanged();
        update();
    }
}

void TimelineWidget::showEntireMedia()
{
    if (model_.showEntireMedia()) {
        emitViewportChanged();
        update();
    }
}

void TimelineWidget::setInPointAtPlayhead()
{
    if (model_.setSelectionIn(model_.playhead())) {
        emitSelectionChanged();
        update();
    }
}

void TimelineWidget::setOutPointAtPlayhead()
{
    if (model_.setSelectionOut(model_.playhead())) {
        emitSelectionChanged();
        update();
    }
}

void TimelineWidget::clearSelection()
{
    if (model_.clearSelection()) {
        emitSelectionChanged();
        update();
    }
}

void TimelineWidget::toggleBookmarkAtPlayhead()
{
    if (!model_.hasMedia()) {
        return;
    }

    const auto playhead = model_.playhead();
    const auto markers = model_.markers();
    const auto existing = std::find_if(markers.begin(), markers.end(), [playhead](const TimelineMarker& marker) {
        return marker.kind == TimelineMarkerKind::Bookmark && marker.time == playhead;
    });
    if (existing != markers.end()) {
        (void)model_.removeMarker(existing->id);
    } else {
        (void)model_.addMarker(playhead, TimelineMarkerKind::Bookmark, tr("Bookmark"));
    }
    update();
}

QRectF TimelineWidget::timelineRect() const noexcept
{
    const qreal availableWidth = std::max<qreal>(1.0, static_cast<qreal>(width()) - 2.0 * kHorizontalInset);
    if (height() < 54) {
        return QRectF(kHorizontalInset, 4.0, availableWidth, std::max<qreal>(1.0, height() - 8.0));
    }

    const qreal bottom = std::max<qreal>(kRulerHeight + 1.0, height() - kBottomAreaHeight);
    return QRectF(
        kHorizontalInset,
        kRulerHeight,
        availableWidth,
        std::max<qreal>(1.0, bottom - kRulerHeight));
}

qint64 TimelineWidget::timeAtX(qreal x) const noexcept
{
    const auto track = timelineRect();
    const auto raw = model_.pixelToTime(x, track.left(), track.width());
    if (const auto marker = markerAtX(x)) {
        return toNanoseconds(marker->time);
    }
    return toNanoseconds(raw);
}

std::optional<TimelineMarker> TimelineWidget::markerAtX(qreal x) const
{
    if (!model_.hasMedia()) {
        return std::nullopt;
    }

    const auto track = timelineRect();
    const auto clampedX = std::clamp(x, track.left(), track.right());
    const auto markers = model_.visibleMarkers();
    std::optional<TimelineMarker> closest;
    qreal closestDistance = kMarkerHitRadius + 1.0;

    // Markers paint in model (time, id) order, so reverse hit testing selects
    // the last-painted/topmost editable marker when coordinates overlap.
    for (auto marker = markers.rbegin(); marker != markers.rend(); ++marker) {
        const qreal markerX = model_.timeToPixel(marker->time, track.left(), track.width());
        const qreal distance = std::abs(markerX - clampedX);
        if (distance <= kMarkerHitRadius && distance < closestDistance) {
            closest = *marker;
            closestDistance = distance;
        }
    }
    if (closest) {
        return closest;
    }

    // Derived keyframes are hit targets only when exact frame boundaries are
    // sparse enough for the model to expose them at the current zoom.
    const auto boundaries = model_.visibleFrameBoundaries(
        track.width(),
        1.0,
        kMaximumFramePrimitives);
    for (const auto& boundary : boundaries) {
        if (!boundary.keyFrame) {
            continue;
        }
        const qreal keyframeX = model_.timeToPixel(boundary.time, track.left(), track.width());
        const qreal distance = std::abs(keyframeX - clampedX);
        if (distance <= kMarkerHitRadius && distance < closestDistance) {
            closest = TimelineMarker{
                0,
                boundary.time,
                TimelineMarkerKind::Keyframe,
                tr("Keyframe"),
            };
            closestDistance = distance;
        }
    }
    return closest;
}

qint64 TimelineWidget::nearestKnownPresentationIndex(media::MediaTime time) const noexcept
{
    const auto frames = model_.knownFrames();
    if (frames.empty()) {
        return -1;
    }

    const auto lower = std::lower_bound(
        frames.begin(),
        frames.end(),
        time,
        [](const FrameBoundary& frame, media::MediaTime target) { return frame.time < target; });

    const FrameBoundary* best = nullptr;
    std::uint64_t bestDistance = std::numeric_limits<std::uint64_t>::max();
    const auto consider = [&](const FrameBoundary& frame) {
        if (frame.id.presentationIndex < 0) {
            return;
        }
        const auto distance = absoluteDistance(frame.time, time);
        if (distance < bestDistance) {
            best = &frame;
            bestDistance = distance;
        }
    };

    if (lower != frames.end()) {
        consider(*lower);
    }
    if (lower != frames.begin()) {
        consider(*std::prev(lower));
    }
    return best ? static_cast<qint64>(best->id.presentationIndex) : -1;
}

void TimelineWidget::emitViewportChanged()
{
    emit viewportChanged(
        toNanoseconds(model_.viewportStart()),
        toNanoseconds(model_.viewportEnd()));
}

void TimelineWidget::emitSelectionChanged()
{
    const auto& selection = model_.selection();
    if (!selection) {
        emit selectionChanged(0, 0, false);
        return;
    }
    emit selectionChanged(
        toNanoseconds(selection->start),
        toNanoseconds(selection->end),
        true);
}

void TimelineWidget::updateHover(QPointF position)
{
    const auto track = timelineRect();
    if (!model_.hasMedia() || !track.contains(position)) {
        if (hoverActive_) {
            hoverActive_ = false;
            emit hoverChanged(hoveredTime_, -1, false);
            emit hoverPreviewChanged(hoveredTime_, -1, QPoint{}, false);
        }
        return;
    }

    const auto time = timeAtX(position.x());
    const auto presentationIndex = nearestKnownPresentationIndex(media::MediaTime(time));
    const QPoint globalPosition = mapToGlobal(position.toPoint());
    if (hoverActive_ && hoveredTime_ == time) {
        emit hoverPreviewChanged(time, presentationIndex, globalPosition, true);
        return;
    }
    hoveredTime_ = time;
    hoverActive_ = true;
    emit hoverChanged(hoveredTime_, presentationIndex, true);
    emit hoverPreviewChanged(hoveredTime_, presentationIndex, globalPosition, true);
}

void TimelineWidget::updateScrub(qreal x, bool force)
{
    const auto requested = timeAtX(x);
    if (!force && requested == lastRequestedTime_) {
        return;
    }
    lastRequestedTime_ = requested;
    (void)model_.setPlayhead(media::MediaTime(requested));
    emit seekRequested(requested);
    update();
}

void TimelineWidget::updateSelection(qreal x)
{
    const auto extent = media::MediaTime(timeAtX(x));
    if (model_.setSelection(selectionAnchor_, extent)) {
        emitSelectionChanged();
        update();
    }
}

void TimelineWidget::cancelInteraction()
{
    const bool wasScrubbing = interaction_ == InteractionMode::Scrub;
    interaction_ = InteractionMode::None;
    lastRequestedTime_ = -1;
    if (wasScrubbing) {
        emit scrubbingChanged(false);
    }
    updateCursorForMode();
}

void TimelineWidget::updateCursorForMode()
{
    if (!isEnabled()) {
        setCursor(Qt::ArrowCursor);
        return;
    }

    switch (interaction_) {
    case InteractionMode::Pan:
        setCursor(Qt::ClosedHandCursor);
        break;
    case InteractionMode::Select:
        setCursor(Qt::CrossCursor);
        break;
    case InteractionMode::ResizeSelectionStart:
    case InteractionMode::ResizeSelectionEnd:
    case InteractionMode::Scrub:
        setCursor(Qt::SizeHorCursor);
        break;
    case InteractionMode::None:
        setCursor(Qt::PointingHandCursor);
        break;
    }
}

void TimelineWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), QColor(25, 29, 36));

    const auto track = timelineRect();
    painter.setPen(QPen(QColor(55, 63, 74), 1.0));
    painter.setBrush(QColor(18, 22, 28));
    painter.drawRoundedRect(track, 4.0, 4.0);

    if (!model_.hasMedia()) {
        painter.setPen(QColor(108, 117, 130));
        painter.drawText(track, Qt::AlignCenter, tr("Timeline"));
        return;
    }

    std::optional<std::size_t> paintedLodLevel;
    if (analysisManager_) {
        const std::size_t pixelBudget = std::clamp<std::size_t>(
            static_cast<std::size_t>(std::ceil(track.width() * devicePixelRatioF())),
            1,
            kMaximumFramePrimitives);
        const auto heatmap = analysisManager_->lodView(
            toNanoseconds(model_.viewportStart()),
            toNanoseconds(model_.viewportEnd()),
            pixelBudget);
        if (!heatmap.buckets.empty()) {
            heatmapRenderer_.paint(
                painter,
                track.adjusted(1.0, 1.0, -1.0, -1.0),
                heatmap,
                heatmapMode_,
                combinedHeatmapWeights_);
            paintedLodLevel = heatmap.level;
        }
    }

    if (const auto& selection = model_.selection()) {
        const qreal selectionStart = model_.timeToPixel(selection->start, track.left(), track.width());
        const qreal selectionEnd = model_.timeToPixel(selection->end, track.left(), track.width());
        const qreal rawLeft = std::min(selectionStart, selectionEnd);
        const qreal rawRight = std::max(selectionStart, selectionEnd);
        if (rawRight >= track.left() && rawLeft <= track.right()) {
            const qreal left = std::clamp(rawLeft, track.left(), track.right());
            const qreal right = std::clamp(rawRight, track.left(), track.right());
            const QRectF selected(left, track.top(), std::max<qreal>(1.0, right - left), track.height());
            painter.setPen(QPen(QColor(79, 164, 255, 210), 1.0));
            painter.setBrush(QColor(54, 132, 220, 48));
            painter.drawRect(selected);
            painter.fillRect(QRectF(left - 2.0, track.top(), 4.0, track.height()), QColor(96, 184, 255, 210));
            painter.fillRect(QRectF(right - 2.0, track.top(), 4.0, track.height()), QColor(96, 184, 255, 210));
        }
    }

    const auto tickInterval = model_.majorTickInterval(track.width());
    const auto majorTicks = model_.majorTicks(track.width());
    painter.save();
    painter.setClipRect(track.adjusted(-1.0, 0.0, 1.0, 0.0));

    for (std::size_t index = 1; index < majorTicks.size(); ++index) {
        const qreal previousX = model_.timeToPixel(majorTicks[index - 1], track.left(), track.width());
        const qreal currentX = model_.timeToPixel(majorTicks[index], track.left(), track.width());
        const qreal spacing = currentX - previousX;
        if (spacing < 25.0) {
            continue;
        }
        painter.setPen(QPen(QColor(66, 74, 86, 90), 1.0));
        for (int minor = 1; minor < 5; ++minor) {
            const qreal x = previousX + spacing * static_cast<qreal>(minor) / 5.0;
            painter.drawLine(QLineF(x, track.bottom() - 7.0, x, track.bottom()));
        }
    }

    painter.setPen(QPen(QColor(72, 82, 96, 150), 1.0));
    for (const auto tick : majorTicks) {
        const qreal x = model_.timeToPixel(tick, track.left(), track.width());
        if (x < track.left() - 1.0 || x > track.right() + 1.0) {
            continue;
        }
        painter.drawLine(QLineF(x, track.top(), x, track.bottom()));
    }

    const auto boundaries = model_.visibleFrameBoundaries(
        track.width(),
        4.0,
        kMaximumFramePrimitives);
    for (const auto& boundary : boundaries) {
        const qreal x = model_.timeToPixel(boundary.time, track.left(), track.width());
        if (x < track.left() || x > track.right()) {
            continue;
        }
        if (boundary.keyFrame) {
            painter.setPen(QPen(QColor(242, 184, 79, 185), 1.0));
            painter.drawLine(QLineF(x, track.top() + 13.0, x, track.bottom()));
            painter.setBrush(QColor(242, 184, 79));
            painter.setPen(Qt::NoPen);
            const QPointF points[] = {
                trianglePoint(x, track.bottom() - 7.0),
                trianglePoint(x - 4.0, track.bottom()),
                trianglePoint(x + 4.0, track.bottom()),
            };
            painter.drawPolygon(points, 3);
        } else {
            painter.setPen(QPen(QColor(126, 143, 164, 115), 1.0));
            painter.drawLine(QLineF(x, track.bottom() - 11.0, x, track.bottom()));
        }
    }

    for (const auto& marker : model_.visibleMarkers()) {
        const qreal x = model_.timeToPixel(marker.time, track.left(), track.width());
        if (x < track.left() || x > track.right()) {
            continue;
        }
        const auto color = markerColor(marker.kind);
        painter.setPen(QPen(QColor(color.red(), color.green(), color.blue(), 190), 1.0));
        painter.drawLine(QLineF(x, track.top(), x, track.bottom()));
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        const QPointF points[] = {
            trianglePoint(x - 5.0, track.top()),
            trianglePoint(x + 5.0, track.top()),
            trianglePoint(x, track.top() + 7.0),
        };
        painter.drawPolygon(points, 3);
    }
    painter.restore();

    painter.setPen(QColor(155, 166, 181));
    const QFontMetricsF metrics(painter.font());
    qreal lastLabelRight = -std::numeric_limits<qreal>::max();
    for (const auto tick : majorTicks) {
        const qreal x = model_.timeToPixel(tick, track.left(), track.width());
        if (x < track.left() - 1.0 || x > track.right() + 1.0) {
            continue;
        }
        const auto label = formatTimelineTime(tick, tickInterval);
        const qreal labelWidth = metrics.horizontalAdvance(label);
        const qreal labelLeft = std::clamp(
            x - labelWidth / 2.0,
            track.left(),
            std::max(track.left(), track.right() - labelWidth));
        if (labelLeft <= lastLabelRight + 5.0) {
            continue;
        }
        painter.drawText(
            QRectF(labelLeft, 3.0, labelWidth + 1.0, std::max<qreal>(1.0, track.top() - 5.0)),
            Qt::AlignLeft | Qt::AlignVCenter,
            label);
        lastLabelRight = labelLeft + labelWidth;
    }

    if (paintedLodLevel) {
        painter.setPen(QColor(190, 199, 212, 205));
        painter.drawText(
            track.adjusted(6.0, 4.0, -6.0, -4.0),
            Qt::AlignTop | Qt::AlignRight,
            tr("%1 heatmap  L%2")
                .arg(heatmapModeName(heatmapMode_))
                .arg(*paintedLodLevel));
    }

    const auto playhead = model_.playhead();
    if (playhead >= model_.viewportStart() && playhead <= model_.viewportEnd()) {
        const qreal x = model_.timeToPixel(playhead, track.left(), track.width());
        painter.setPen(QPen(QColor(255, 92, 105), 1.5));
        painter.drawLine(QLineF(x, track.top() - 2.0, x, track.bottom()));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 92, 105));
        const QPointF points[] = {
            trianglePoint(x - 5.0, track.top() - 3.0),
            trianglePoint(x + 5.0, track.top() - 3.0),
            trianglePoint(x, track.top() + 4.0),
        };
        painter.drawPolygon(points, 3);
    }

    if (const auto& selection = model_.selection()) {
        const auto details = model_.selectionDetails();
        QString frameText;
        if (details.frameCount) {
            frameText = tr("%1 frames").arg(*details.frameCount);
        } else {
            frameText = tr("%1 known frames").arg(details.knownFrameCount);
        }
        const auto detailText = tr("Selection %1 -> %2  |  %3")
                                    .arg(formatTimelineTime(selection->start, 1ms))
                                    .arg(formatTimelineTime(selection->end, 1ms))
                                    .arg(frameText);
        painter.setPen(QColor(141, 177, 218));
        painter.drawText(
            QRectF(track.left(), track.bottom() + 2.0, track.width(), 17.0),
            Qt::AlignLeft | Qt::AlignVCenter,
            metrics.elidedText(detailText, Qt::ElideRight, static_cast<int>(track.width())));
    }

    const QRectF overview(
        track.left(),
        std::max(track.bottom() + 19.0, static_cast<qreal>(height()) - kOverviewBottomInset - kOverviewHeight),
        track.width(),
        kOverviewHeight);
    painter.setPen(QPen(QColor(58, 66, 78), 1.0));
    painter.setBrush(QColor(14, 17, 22));
    painter.drawRoundedRect(overview, 2.0, 2.0);

    const auto durationCount = model_.duration().count();
    const auto overviewX = [&](media::MediaTime time) {
        if (durationCount <= 0) {
            return overview.left();
        }
        const long double ratio = std::clamp(
            static_cast<long double>(time.count()) / static_cast<long double>(durationCount),
            0.0L,
            1.0L);
        return overview.left() + overview.width() * static_cast<qreal>(ratio);
    };

    if (const auto& selection = model_.selection()) {
        const qreal left = overviewX(selection->start);
        const qreal right = overviewX(selection->end);
        painter.fillRect(
            QRectF(left, overview.top() + 1.0, std::max<qreal>(1.0, right - left), overview.height() - 2.0),
            QColor(54, 132, 220, 85));
    }

    int lastOverviewMarkerPixel = std::numeric_limits<int>::min();
    for (const auto& marker : model_.markers()) {
        const qreal x = overviewX(marker.time);
        const int pixel = static_cast<int>(std::lround(x));
        if (pixel == lastOverviewMarkerPixel) {
            continue;
        }
        lastOverviewMarkerPixel = pixel;
        painter.setPen(QPen(markerColor(marker.kind), 1.0));
        painter.drawLine(QLineF(x, overview.top() + 1.0, x, overview.bottom() - 1.0));
    }

    const qreal viewportLeft = overviewX(model_.viewportStart());
    const qreal viewportRight = overviewX(model_.viewportEnd());
    const QRectF viewportIndicator(
        viewportLeft,
        overview.top(),
        std::max<qreal>(2.0, viewportRight - viewportLeft),
        overview.height());
    painter.setPen(QPen(QColor(91, 177, 255, 220), 1.0));
    painter.setBrush(QColor(61, 143, 224, 50));
    painter.drawRoundedRect(viewportIndicator, 2.0, 2.0);

    painter.setPen(QPen(QColor(255, 92, 105), 1.0));
    const qreal overviewPlayhead = overviewX(playhead);
    painter.drawLine(QLineF(
        overviewPlayhead,
        overview.top() - 1.0,
        overviewPlayhead,
        overview.bottom() + 1.0));

    if (hasFocus()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(75, 169, 255, 130), 1.0));
        painter.drawRoundedRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0), 5.0, 5.0);
    }
}

void TimelineWidget::mousePressEvent(QMouseEvent* event)
{
    if (interaction_ != InteractionMode::None) {
        event->accept();
        return;
    }

    const auto track = timelineRect();
    if (!model_.hasMedia() || !track.contains(event->position())) {
        QWidget::mousePressEvent(event);
        return;
    }

    setFocus(Qt::MouseFocusReason);
    pressPosition_ = event->position();

    if (event->button() == Qt::MiddleButton) {
        interaction_ = InteractionMode::Pan;
        panStart_ = model_.viewportStart();
        panEnd_ = model_.viewportEnd();
        updateCursorForMode();
        event->accept();
        return;
    }

    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    if (event->modifiers().testFlag(Qt::ShiftModifier)) {
        const auto& selection = model_.selection();
        if (selection) {
            const qreal startX = model_.timeToPixel(selection->start, track.left(), track.width());
            const qreal endX = model_.timeToPixel(selection->end, track.left(), track.width());
            const qreal startDistance = std::abs(event->position().x() - startX);
            const qreal endDistance = std::abs(event->position().x() - endX);
            if (startDistance <= kSelectionHandleRadius && startDistance <= endDistance) {
                interaction_ = InteractionMode::ResizeSelectionStart;
                selectionAnchor_ = selection->end;
            } else if (endDistance <= kSelectionHandleRadius) {
                interaction_ = InteractionMode::ResizeSelectionEnd;
                selectionAnchor_ = selection->start;
            } else {
                interaction_ = InteractionMode::Select;
                selectionAnchor_ = media::MediaTime(timeAtX(event->position().x()));
            }
        } else {
            interaction_ = InteractionMode::Select;
            selectionAnchor_ = media::MediaTime(timeAtX(event->position().x()));
        }
        updateSelection(event->position().x());
        updateCursorForMode();
        event->accept();
        return;
    }

    if (const auto marker = markerAtX(event->position().x()); marker && marker->id != 0) {
        emit markerActivated(
            static_cast<quint64>(marker->id),
            toNanoseconds(marker->time));
    }
    interaction_ = InteractionMode::Scrub;
    lastRequestedTime_ = -1;
    updateCursorForMode();
    emit scrubbingChanged(true);
    updateScrub(event->position().x(), true);
    event->accept();
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event)
{
    updateHover(event->position());

    switch (interaction_) {
    case InteractionMode::Scrub:
        if (!event->buttons().testFlag(Qt::LeftButton)) {
            cancelInteraction();
            event->accept();
            return;
        }
        updateScrub(event->position().x(), false);
        event->accept();
        return;
    case InteractionMode::Select:
    case InteractionMode::ResizeSelectionStart:
    case InteractionMode::ResizeSelectionEnd:
        if (!event->buttons().testFlag(Qt::LeftButton)) {
            cancelInteraction();
            event->accept();
            return;
        }
        updateSelection(event->position().x());
        event->accept();
        return;
    case InteractionMode::Pan: {
        if (!event->buttons().testFlag(Qt::MiddleButton)) {
            cancelInteraction();
            event->accept();
            return;
        }
        const auto previousStart = model_.viewportStart();
        const auto previousEnd = model_.viewportEnd();
        (void)model_.setViewport(panStart_, panEnd_);
        (void)model_.panByPixels(
            pressPosition_.x() - event->position().x(),
            timelineRect().width());
        if (model_.viewportStart() != previousStart || model_.viewportEnd() != previousEnd) {
            emitViewportChanged();
            update();
        }
        event->accept();
        return;
    }
    case InteractionMode::None:
        break;
    }
    QWidget::mouseMoveEvent(event);
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (interaction_ == InteractionMode::Scrub && event->button() == Qt::LeftButton) {
        updateScrub(event->position().x(), false);
        cancelInteraction();
        event->accept();
        return;
    }

    if ((interaction_ == InteractionMode::Select
         || interaction_ == InteractionMode::ResizeSelectionStart
         || interaction_ == InteractionMode::ResizeSelectionEnd)
        && event->button() == Qt::LeftButton) {
        updateSelection(event->position().x());
        cancelInteraction();
        event->accept();
        return;
    }

    if (interaction_ == InteractionMode::Pan && event->button() == Qt::MiddleButton) {
        const auto previousStart = model_.viewportStart();
        const auto previousEnd = model_.viewportEnd();
        (void)model_.setViewport(panStart_, panEnd_);
        (void)model_.panByPixels(
            pressPosition_.x() - event->position().x(),
            timelineRect().width());
        if (model_.viewportStart() != previousStart || model_.viewportEnd() != previousEnd) {
            emitViewportChanged();
            update();
        }
        cancelInteraction();
        event->accept();
        return;
    }

    QWidget::mouseReleaseEvent(event);
}

void TimelineWidget::leaveEvent(QEvent* event)
{
    if (hoverActive_) {
        hoverActive_ = false;
        emit hoverChanged(hoveredTime_, -1, false);
        emit hoverPreviewChanged(hoveredTime_, -1, QPoint{}, false);
    }
    QWidget::leaveEvent(event);
}

void TimelineWidget::focusOutEvent(QFocusEvent* event)
{
    cancelInteraction();
    QWidget::focusOutEvent(event);
}

void TimelineWidget::wheelEvent(QWheelEvent* event)
{
    if (!model_.hasMedia()) {
        QWidget::wheelEvent(event);
        return;
    }

    const auto track = timelineRect();
    const qreal x = std::clamp(event->position().x(), track.left(), track.right());
    const bool inverted = event->inverted();

    if (event->modifiers().testFlag(Qt::ShiftModifier)) {
        double wheelPixels = 0.0;
        if (!event->pixelDelta().isNull()) {
            wheelPixels = event->pixelDelta().x() != 0
                ? static_cast<double>(event->pixelDelta().x())
                : static_cast<double>(event->pixelDelta().y());
        } else {
            const int angle = event->angleDelta().x() != 0
                ? event->angleDelta().x()
                : event->angleDelta().y();
            wheelPixels = static_cast<double>(angle) / 120.0 * track.width() * 0.1;
        }
        if (inverted) {
            wheelPixels = -wheelPixels;
        }
        if (model_.panByPixels(-wheelPixels, track.width())) {
            emitViewportChanged();
            update();
        }
    } else {
        double steps = 0.0;
        if (!event->pixelDelta().isNull()) {
            steps = static_cast<double>(event->pixelDelta().y()) / 120.0;
        } else {
            steps = static_cast<double>(event->angleDelta().y()) / 120.0;
        }
        if (inverted) {
            steps = -steps;
        }
        if (steps != 0.0) {
            const double factor = std::pow(kZoomStep, steps);
            const auto anchor = model_.pixelToTime(x, track.left(), track.width());
            if (std::isfinite(factor) && factor > 0.0 && model_.zoomAt(factor, anchor)) {
                emitViewportChanged();
                update();
            }
        }
    }

    updateHover(event->position());
    event->accept();
}

} // namespace vidscope::timeline
