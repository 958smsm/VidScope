#pragma once

#include <QtGui/QImage>
#include <QtWidgets/QWidget>

namespace vidscope::render {

class VideoViewport final : public QWidget {
    Q_OBJECT

public:
    explicit VideoViewport(QWidget* parent = nullptr);
    void setFrame(const QImage& frame);
    void clearFrame();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QRect fittedFrameRect() const;
    QImage frame_;
};

} // namespace vidscope::render
