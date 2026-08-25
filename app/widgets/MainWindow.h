#pragma once

#include "media/MediaTypes.h"
#include "playback/PlaybackController.h"

#include <QtWidgets/QMainWindow>

class QLabel;
class QComboBox;
class QDockWidget;
class QToolButton;

namespace vidscope::analysis {
class AnalysisManager;
}

namespace vidscope::render {
class VideoViewport;
}

namespace vidscope::timeline {
class TimelineWidget;
}

namespace vidscope::thumbnails {
class ThumbnailManager;
}

namespace vidscope::widgets {

class FilmstripController;
class FilmstripWidget;
class HoverPreviewController;
class AnalysisResultsPanel;

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
    void updateSelectionStatus();
    void applyDetectionResults();
    void seekAdjacentScene(bool forward);
    void showShortcutEditor();
    [[nodiscard]] int frameStepCount() const;

    bool opening_ = false;
    playback::PlaybackController* controller_ = nullptr;
    analysis::AnalysisManager* analysisManager_ = nullptr;
    thumbnails::ThumbnailManager* thumbnailManager_ = nullptr;
    FilmstripController* filmstripController_ = nullptr;
    HoverPreviewController* hoverPreviewController_ = nullptr;
    render::VideoViewport* viewport_ = nullptr;
    timeline::TimelineWidget* timeline_ = nullptr;
    FilmstripWidget* filmstrip_ = nullptr;
    AnalysisResultsPanel* analysisResults_ = nullptr;
    QDockWidget* analysisResultsDock_ = nullptr;
    QLabel* frameStatus_ = nullptr;
    QLabel* mediaStatus_ = nullptr;
    QLabel* selectionStatus_ = nullptr;
    QLabel* analysisStatus_ = nullptr;
    QComboBox* frameStepBox_ = nullptr;
    QComboBox* filmstripModeBox_ = nullptr;
    QComboBox* filmstripCountBox_ = nullptr;
    QToolButton* playPauseButton_ = nullptr;
};

} // namespace vidscope::widgets
