#pragma once

#include <QtWidgets/QWidget>

namespace vidscope::widgets {

class SeekBar final : public QWidget {
    Q_OBJECT

public:
    explicit SeekBar(QWidget* parent = nullptr);
    void setDuration(qint64 nanoseconds);
    void setPosition(qint64 nanoseconds);

signals:
    void seekRequested(qint64 nanoseconds);
    void scrubbingChanged(bool active);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] qint64 timeAtX(qreal x) const noexcept;
    qint64 duration_ = 0;
    qint64 position_ = 0;
    bool scrubbing_ = false;
};

} // namespace vidscope::widgets
