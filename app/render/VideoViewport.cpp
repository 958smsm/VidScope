#include "render/VideoViewport.h"

#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QSizePolicy>

namespace vidscope::render {

VideoViewport::VideoViewport(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("videoViewport"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    setMinimumSize(480, 270);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void VideoViewport::setFrame(const QImage& frame)
{
    frame_ = frame;
    update();
}

void VideoViewport::clearFrame()
{
    if (frame_.isNull()) {
        return;
    }
    frame_ = {};
    update();
}

QRect VideoViewport::fittedFrameRect() const
{
    if (frame_.isNull() || width() <= 0 || height() <= 0) {
        return {};
    }

    const auto fitted = frame_.size().scaled(size(), Qt::KeepAspectRatio);
    return QRect(
        (width() - fitted.width()) / 2,
        (height() - fitted.height()) / 2,
        fitted.width(),
        fitted.height());
}

void VideoViewport::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.fillRect(rect(), QColor(7, 9, 12));

    if (frame_.isNull()) {
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF panel = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
        painter.setPen(QPen(QColor(38, 43, 51), 1.0));
        painter.drawRoundedRect(panel, 5.0, 5.0);

        auto font = painter.font();
        font.setPointSizeF(font.pointSizeF() + 1.0);
        painter.setFont(font);
        painter.setPen(QColor(116, 124, 138));
        painter.drawText(rect(), Qt::AlignCenter, tr("Open a video to begin inspection"));
        return;
    }

    const QRect destination = fittedFrameRect();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(destination, frame_);

    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(QColor(255, 255, 255, 28), 1.0));
    painter.drawRect(destination.adjusted(0, 0, -1, -1));
}

void VideoViewport::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    update();
}

} // namespace vidscope::render
