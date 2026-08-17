#pragma once

#include "media/MediaTypes.h"
#include "playback/PlaybackController.h"

#include <QtWidgets/QMainWindow>

class QLabel;
class QComboBox;
class QToolButton;

namespace vidscope::render {
class VideoViewport;
}

namespace vidscope::widgets {

class SeekBar;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private slots:
    void openFile();
    void handleMediaOpened(media::MediaInfoPtr info);
    void handleFrame(media::DecodedFramePtr frame, const QImage& image);
    void handleState(playback::PlaybackState state);
    void showPlaybackError(const QString& title, const QString& detail);

private:
    void createActions();
    void createLayout();
    void createMenus();
    void createShortcuts();
    void updateFrameStatus(const media::DecodedFrame& frame);
    [[nodiscard]] int frameStepCount() const;

    playback::PlaybackController* controller_ = nullptr;
    render::VideoViewport* viewport_ = nullptr;
    SeekBar* seekBar_ = nullptr;
    QLabel* frameStatus_ = nullptr;
    QLabel* mediaStatus_ = nullptr;
    QComboBox* frameStepBox_ = nullptr;
    QToolButton* playPauseButton_ = nullptr;
};

} // namespace vidscope::widgets
