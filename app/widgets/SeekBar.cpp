#include "widgets/SeekBar.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>

#include <algorithm>
#include <cmath>

namespace vidscope::widgets {
namespace {

constexpr qreal kHorizontalInset = 11.0;
constexpr qreal kTrackHeight = 5.0;
constexpr qreal kHandleRadius = 6.0;

} // namespace

SeekBar::SeekBar(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("seekBar"));
    setAccessibleName(tr("Video position"));
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumHeight(28);
    setMaximumHeight(32);
    setEnabled(false);
}

void SeekBar::setDuration(qint64 nanoseconds)
{
    duration_ = std::max<qint64>(0, nanoseconds);
    position_ = std::clamp(position_, qint64{0}, duration_);
    setEnabled(duration_ > 0);
    update();
}

void SeekBar::setPosition(qint64 nanoseconds)
{
    if (scrubbing_) {
        return;
    }
    const auto position = std::clamp(nanoseconds, qint64{0}, duration_);
    if (position_ == position) {
        return;
    }
    position_ = position;
    update();
}

qint64 SeekBar::timeAtX(qreal x) const noexcept
{
    if (duration_ <= 0) {
        return 0;
    }
    const qreal usableWidth = std::max<qreal>(1.0, static_cast<qreal>(width()) - 2.0 * kHorizontalInset);
    const qreal ratio = std::clamp((x - kHorizontalInset) / usableWidth, 0.0, 1.0);
    return static_cast<qint64>(
        std::llround(static_cast<long double>(duration_) * static_cast<long double>(ratio)));
}

void SeekBar::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const qreal usableWidth = std::max<qreal>(1.0, static_cast<qreal>(width()) - 2.0 * kHorizontalInset);
    const qreal centerY = static_cast<qreal>(height()) / 2.0;
    const QRectF track(
        kHorizontalInset,
        centerY - kTrackHeight / 2.0,
        usableWidth,
        kTrackHeight);

    painter.setPen(Qt::NoPen);
    painter.setBrush(isEnabled() ? QColor(51, 57, 67) : QColor(38, 42, 49));
    painter.drawRoundedRect(track, kTrackHeight / 2.0, kTrackHeight / 2.0);

    const qreal ratio = duration_ > 0
        ? std::clamp(
              static_cast<qreal>(position_) / static_cast<qreal>(duration_),
              0.0,
              1.0)
        : 0.0;
    if (ratio > 0.0) {
        QRectF elapsed = track;
        elapsed.setWidth(std::max<qreal>(kTrackHeight, usableWidth * ratio));
        QLinearGradient gradient(elapsed.topLeft(), elapsed.topRight());
        gradient.setColorAt(0.0, QColor(52, 134, 246));
        gradient.setColorAt(1.0, QColor(75, 169, 255));
        painter.setBrush(gradient);
        painter.drawRoundedRect(elapsed, kTrackHeight / 2.0, kTrackHeight / 2.0);
    }

    const qreal handleX = kHorizontalInset + usableWidth * ratio;
    painter.setBrush(isEnabled() ? QColor(226, 239, 255) : QColor(95, 101, 111));
    painter.setPen(QPen(QColor(15, 20, 27, 180), 1.0));
    painter.drawEllipse(QPointF(handleX, centerY), kHandleRadius, kHandleRadius);

    if (hasFocus() && isEnabled()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(75, 169, 255, 130), 1.0));
        painter.drawRoundedRect(rect().adjusted(1, 1, -2, -2), 4.0, 4.0);
    }
}

void SeekBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton || duration_ <= 0) {
        QWidget::mousePressEvent(event);
        return;
    }

    setFocus(Qt::MouseFocusReason);
    scrubbing_ = true;
    position_ = timeAtX(event->position().x());
    update();
    emit scrubbingChanged(true);
    emit seekRequested(position_);
    event->accept();
}

void SeekBar::mouseMoveEvent(QMouseEvent* event)
{
    if (!scrubbing_ || !(event->buttons() & Qt::LeftButton)) {
        QWidget::mouseMoveEvent(event);
        return;
    }

    const auto position = timeAtX(event->position().x());
    if (position != position_) {
        position_ = position;
        update();
        emit seekRequested(position_);
    }
    event->accept();
}

void SeekBar::mouseReleaseEvent(QMouseEvent* event)
{
    if (!scrubbing_ || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }

    position_ = timeAtX(event->position().x());
    update();
    emit seekRequested(position_);
    scrubbing_ = false;
    emit scrubbingChanged(false);
    event->accept();
}

} // namespace vidscope::widgets
