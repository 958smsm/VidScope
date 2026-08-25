#include "render/VideoViewport.h"

#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QSizePolicy>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace vidscope::render {

VideoViewport::VideoViewport(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("videoViewport"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    setMouseTracking(true);
    setMinimumSize(480, 270);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    blinkTimer_.setInterval(500);
    connect(&blinkTimer_, &QTimer::timeout, this, [this] {
        blinkShowsA_ = !blinkShowsA_;
        update();
    });
}

void VideoViewport::setFrame(const QImage& frame)
{
    frame_ = frame;
    update();
}

void VideoViewport::clearFrame()
{
    if (frame_.isNull() && !comparisonActive_) {
        return;
    }
    frame_ = {};
    clearComparison();
    update();
}

void VideoViewport::setImageZoom(const double factor)
{
    const double normalized = std::isfinite(factor) && factor >= 0.25
        ? std::clamp(factor, 0.25, 16.0)
        : 0.0;
    if (imageZoom_ == normalized) {
        return;
    }
    imageZoom_ = normalized;
    update();
}

double VideoViewport::imageZoom() const noexcept
{
    return imageZoom_;
}

void VideoViewport::setPixelInspectionEnabled(const bool enabled)
{
    if (pixelInspectionEnabled_ == enabled) {
        return;
    }
    pixelInspectionEnabled_ = enabled;
    if (!enabled) {
        emit pixelInspectionLeft();
    }
    setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
}

bool VideoViewport::pixelInspectionEnabled() const noexcept
{
    return pixelInspectionEnabled_;
}

void VideoViewport::setComparison(
    const QImage& frameA,
    const QImage& frameB,
    const inspection::ComparisonMode mode,
    const QImage& visualization,
    QString detail)
{
    if (frameA.isNull() || frameB.isNull()) {
        clearComparison();
        return;
    }
    frameA_ = frameA;
    frameB_ = frameB;
    comparisonVisualization_ = visualization;
    comparisonDetail_ = std::move(detail);
    comparisonMode_ = mode;
    comparisonActive_ = true;
    blinkShowsA_ = true;
    updateBlinkTimer();
    emit pixelInspectionLeft();
    update();
}

void VideoViewport::clearComparison()
{
    if (!comparisonActive_ && frameA_.isNull() && frameB_.isNull()) {
        return;
    }
    comparisonActive_ = false;
    frameA_ = {};
    frameB_ = {};
    comparisonVisualization_ = {};
    comparisonDetail_.clear();
    blinkTimer_.stop();
    update();
}

bool VideoViewport::comparisonActive() const noexcept
{
    return comparisonActive_;
}

inspection::ComparisonMode VideoViewport::comparisonMode() const noexcept
{
    return comparisonMode_;
}

QRect VideoViewport::fittedRect(const QImage& image, const QRect& bounds) const
{
    if (image.isNull() || bounds.width() <= 0 || bounds.height() <= 0) {
        return {};
    }
    const auto fitted = image.size().scaled(bounds.size(), Qt::KeepAspectRatio);
    return QRect(
        bounds.left() + (bounds.width() - fitted.width()) / 2,
        bounds.top() + (bounds.height() - fitted.height()) / 2,
        fitted.width(),
        fitted.height());
}

QRect VideoViewport::displayedFrameRect() const
{
    if (frame_.isNull()) {
        return {};
    }
    if (imageZoom_ == 0.0) {
        return fittedRect(frame_, rect());
    }
    const int scaledWidth = std::max(
        1,
        static_cast<int>(std::min(
            static_cast<double>(std::numeric_limits<int>::max()),
            static_cast<double>(frame_.width()) * imageZoom_)));
    const int scaledHeight = std::max(
        1,
        static_cast<int>(std::min(
            static_cast<double>(std::numeric_limits<int>::max()),
            static_cast<double>(frame_.height()) * imageZoom_)));
    return QRect(
        (width() - scaledWidth) / 2,
        (height() - scaledHeight) / 2,
        scaledWidth,
        scaledHeight);
}

void VideoViewport::updateBlinkTimer()
{
    if (comparisonActive_ && comparisonMode_ == inspection::ComparisonMode::Blink) {
        blinkTimer_.start();
    } else {
        blinkTimer_.stop();
    }
}

void VideoViewport::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.fillRect(rect(), QColor(7, 9, 12));

    if (frame_.isNull() && !comparisonActive_) {
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

    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (comparisonActive_) {
        switch (comparisonMode_) {
        case inspection::ComparisonMode::SideBySide: {
            const int gap = 6;
            const int halfWidth = std::max(1, (width() - gap) / 2);
            const QRect leftBounds(0, 0, halfWidth, height());
            const QRect rightBounds(halfWidth + gap, 0, width() - halfWidth - gap, height());
            painter.drawImage(fittedRect(frameA_, leftBounds), frameA_);
            painter.drawImage(fittedRect(frameB_, rightBounds), frameB_);
            painter.setPen(QColor(235, 239, 246));
            painter.drawText(leftBounds.adjusted(8, 8, -8, -8), Qt::AlignLeft, tr("A"));
            painter.drawText(rightBounds.adjusted(8, 8, -8, -8), Qt::AlignLeft, tr("B"));
            break;
        }
        case inspection::ComparisonMode::Overlay: {
            const QRect destinationA = fittedRect(frameA_, rect());
            const QRect destinationB = fittedRect(frameB_, rect());
            painter.drawImage(destinationA, frameA_);
            painter.setOpacity(0.5);
            painter.drawImage(destinationB, frameB_);
            painter.setOpacity(1.0);
            break;
        }
        case inspection::ComparisonMode::Wipe: {
            const QRect destinationA = fittedRect(frameA_, rect());
            const QRect destinationB = fittedRect(frameB_, rect());
            painter.drawImage(destinationA, frameA_);
            const int wipeX = rect().center().x();
            painter.save();
            painter.setClipRect(QRect(wipeX, 0, width() - wipeX, height()));
            painter.drawImage(destinationB, frameB_);
            painter.restore();
            painter.setPen(QPen(QColor(255, 255, 255, 190), 2.0));
            painter.drawLine(wipeX, 0, wipeX, height());
            break;
        }
        case inspection::ComparisonMode::Blink: {
            const QImage& selected = blinkShowsA_ ? frameA_ : frameB_;
            painter.drawImage(fittedRect(selected, rect()), selected);
            painter.setPen(QColor(235, 239, 246));
            painter.drawText(
                rect().adjusted(8, 8, -8, -8),
                Qt::AlignLeft | Qt::AlignTop,
                blinkShowsA_ ? tr("A") : tr("B"));
            break;
        }
        case inspection::ComparisonMode::AbsoluteDifference:
        case inspection::ComparisonMode::AmplifiedDifference:
        case inspection::ComparisonMode::SsimMap:
            if (!comparisonVisualization_.isNull()) {
                painter.drawImage(
                    fittedRect(comparisonVisualization_, rect()),
                    comparisonVisualization_);
            } else {
                painter.setPen(QColor(155, 166, 181));
                painter.drawText(
                    rect().adjusted(24, 24, -24, -24),
                    Qt::AlignCenter | Qt::TextWordWrap,
                    comparisonDetail_.isEmpty()
                        ? tr("Comparison unavailable")
                        : comparisonDetail_);
            }
            break;
        }
    } else {
        const QRect destination = displayedFrameRect();
        painter.drawImage(destination, frame_);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(255, 255, 255, 28), 1.0));
        painter.drawRect(destination.adjusted(0, 0, -1, -1));
    }
}

void VideoViewport::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    update();
}

void VideoViewport::mouseMoveEvent(QMouseEvent* event)
{
    if (!pixelInspectionEnabled_ || comparisonActive_ || frame_.isNull()) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const QRect destination = displayedFrameRect();
    if (!destination.contains(event->position().toPoint())
        || destination.width() <= 0 || destination.height() <= 0) {
        emit pixelInspectionLeft();
        QWidget::mouseMoveEvent(event);
        return;
    }
    const double relativeX =
        (event->position().x() - static_cast<double>(destination.left()))
        / static_cast<double>(destination.width());
    const double relativeY =
        (event->position().y() - static_cast<double>(destination.top()))
        / static_cast<double>(destination.height());
    const int x = std::clamp(
        static_cast<int>(std::floor(relativeX * static_cast<double>(frame_.width()))),
        0,
        frame_.width() - 1);
    const int y = std::clamp(
        static_cast<int>(std::floor(relativeY * static_cast<double>(frame_.height()))),
        0,
        frame_.height() - 1);
    emit pixelInspected(x, y, frame_.pixelColor(x, y));
    QWidget::mouseMoveEvent(event);
}

void VideoViewport::leaveEvent(QEvent* event)
{
    emit pixelInspectionLeft();
    QWidget::leaveEvent(event);
}

} // namespace vidscope::render
