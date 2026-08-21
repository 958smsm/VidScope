#pragma once

#include "timeline/TimelineModel.h"

#include <QtCore/QPoint>
#include <QtCore/QPointF>
#include <QtWidgets/QWidget>

#include <cstdint>
#include <optional>

namespace vidscope::timeline {

class TimelineWidget final : public QWidget {
    Q_OBJECT

public:
    explicit TimelineWidget(QWidget* parent = nullptr);

    void setDuration(qint64 nanoseconds);
    void setPosition(qint64 nanoseconds);
    void observeFrame(const media::DecodedFrame& frame);

    [[nodiscard]] const TimelineModel& model() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> addMarker(
        qint64 nanoseconds,
        TimelineMarkerKind kind,
        QString label = {});
    bool removeMarker(std::uint64_t id);
    void clearMarkers(std::optional<TimelineMarkerKind> kind = std::nullopt);
    [[nodiscard]] std::optional<qint64> adjacentMarkerNanoseconds(
        TimelineMarkerKind kind,
        bool forward) const noexcept;

public slots:
    void zoomIn();
    void zoomOut();
    void showEntireMedia();
    void setInPointAtPlayhead();
    void setOutPointAtPlayhead();
    void clearSelection();
    void toggleBookmarkAtPlayhead();

signals:
    void seekRequested(qint64 nanoseconds);
    void scrubbingChanged(bool active);
    void viewportChanged(qint64 startNanoseconds, qint64 endNanoseconds);
    void selectionChanged(qint64 startNanoseconds, qint64 endNanoseconds, bool active);
    void hoverChanged(qint64 nanoseconds, qint64 presentationIndex, bool active);
    void hoverPreviewChanged(
        qint64 nanoseconds,
        qint64 presentationIndex,
        QPoint globalPosition,
        bool active);
    void markerActivated(quint64 id, qint64 nanoseconds);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    enum class InteractionMode : std::uint8_t {
        None,
        Scrub,
        Pan,
        Select,
        ResizeSelectionStart,
        ResizeSelectionEnd,
    };

    [[nodiscard]] QRectF timelineRect() const noexcept;
    [[nodiscard]] qint64 timeAtX(qreal x) const noexcept;
    [[nodiscard]] std::optional<TimelineMarker> markerAtX(qreal x) const;
    [[nodiscard]] qint64 nearestKnownPresentationIndex(media::MediaTime time) const noexcept;
    void emitViewportChanged();
    void emitSelectionChanged();
    void updateHover(QPointF position);
    void updateScrub(qreal x, bool force);
    void updateSelection(qreal x);
    void cancelInteraction();
    void updateCursorForMode();

    TimelineModel model_;
    InteractionMode interaction_ = InteractionMode::None;
    QPointF pressPosition_;
    media::MediaTime selectionAnchor_{};
    media::MediaTime panStart_{};
    media::MediaTime panEnd_{};
    qint64 lastRequestedTime_ = -1;
    qint64 hoveredTime_ = 0;
    bool hoverActive_ = false;
};

} // namespace vidscope::timeline
