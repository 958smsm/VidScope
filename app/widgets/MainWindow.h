#pragma once

#include "inspection/FrameHistory.h"
#include "media/MediaTypes.h"
#include "playback/PlaybackController.h"
#include "timeline/TimelineModel.h"

#include <QtWidgets/QMainWindow>

#include <cstdint>

class QLabel;
class QComboBox;
class QDockWidget;
class QProgressDialog;
class QToolButton;

namespace vidscope::analysis {
class AnalysisManager;
}

namespace vidscope::render {
class VideoViewport;
}

namespace vidscope::exporting {
class ExportManager;
enum class RelativeFrame : std::int8_t;
struct ExportRequest;
struct ExportSummary;
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
class FrameInspectorPanel;
class ProfessionalPanel;

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
    void seekAdjacentMarker(timeline::TimelineMarkerKind kind, bool forward);
    void navigateHistory(bool forward);
    void showMarkerEditor(std::optional<std::uint64_t> markerId = std::nullopt);
    void findVisualMatches();
    void updateProfessionalTools();
    void showShortcutEditor();
    void exportSingleFrame(exporting::RelativeFrame relativeFrame);
    void exportSelectedFrames();
    void exportEveryNFrames();
    void exportKeyframes();
    void exportSceneFrames();
    void exportHighMotionFrames();
    void createContactSheet();
    void startExport(exporting::ExportRequest request);
    void finishExport(const exporting::ExportSummary& summary);
    void updateExportActions();
    [[nodiscard]] int frameStepCount() const;

    bool opening_ = false;
    playback::PlaybackController* controller_ = nullptr;
    analysis::AnalysisManager* analysisManager_ = nullptr;
    exporting::ExportManager* exportManager_ = nullptr;
    thumbnails::ThumbnailManager* thumbnailManager_ = nullptr;
    FilmstripController* filmstripController_ = nullptr;
    HoverPreviewController* hoverPreviewController_ = nullptr;
    render::VideoViewport* viewport_ = nullptr;
    timeline::TimelineWidget* timeline_ = nullptr;
    FilmstripWidget* filmstrip_ = nullptr;
    AnalysisResultsPanel* analysisResults_ = nullptr;
    QDockWidget* analysisResultsDock_ = nullptr;
    FrameInspectorPanel* frameInspector_ = nullptr;
    QDockWidget* frameInspectorDock_ = nullptr;
    ProfessionalPanel* professionalPanel_ = nullptr;
    QDockWidget* professionalDock_ = nullptr;
    inspection::FrameHistory frameHistory_;
    media::DecodedFramePtr currentFrame_;
    media::MediaInfoPtr mediaInfo_;
    QProgressDialog* exportProgress_ = nullptr;
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
