#pragma once

#include "inspection/FrameComparison.h"

#include <QtCore/QTimer>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtWidgets/QWidget>

namespace vidscope::render {

class VideoViewport final : public QWidget {
    Q_OBJECT

public:
    explicit VideoViewport(QWidget* parent = nullptr);
    void setFrame(const QImage& frame);
    void clearFrame();
    void setImageZoom(double factor);
    [[nodiscard]] double imageZoom() const noexcept;
    void setPixelInspectionEnabled(bool enabled);
    [[nodiscard]] bool pixelInspectionEnabled() const noexcept;
    void setComparison(
        const QImage& frameA,
        const QImage& frameB,
        inspection::ComparisonMode mode,
        const QImage& visualization = {},
        QString detail = {});
    void clearComparison();
    [[nodiscard]] bool comparisonActive() const noexcept;
    [[nodiscard]] inspection::ComparisonMode comparisonMode() const noexcept;

signals:
    void pixelInspected(int x, int y, QColor rgb);
    void pixelInspectionLeft();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    [[nodiscard]] QRect fittedRect(const QImage& image, const QRect& bounds) const;
    [[nodiscard]] QRect displayedFrameRect() const;
    void updateBlinkTimer();

    QImage frame_;
    QImage frameA_;
    QImage frameB_;
    QImage comparisonVisualization_;
    QString comparisonDetail_;
    inspection::ComparisonMode comparisonMode_ =
        inspection::ComparisonMode::SideBySide;
    QTimer blinkTimer_;
    double imageZoom_ = 0.0;
    bool comparisonActive_ = false;
    bool pixelInspectionEnabled_ = false;
    bool blinkShowsA_ = true;
};

} // namespace vidscope::render
