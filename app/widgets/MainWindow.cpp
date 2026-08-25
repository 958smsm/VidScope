#include "widgets/MainWindow.h"

#include "analysis/AnalysisManager.h"
#include "export/ExportManager.h"
#include "render/VideoViewport.h"
#include "timeline/TimelineWidget.h"
#include "thumbnails/ThumbnailManager.h"
#include "widgets/FilmstripController.h"
#include "widgets/FilmstripWidget.h"
#include "widgets/FrameInspectorPanel.h"
#include "widgets/HoverPreviewController.h"
#include "widgets/AnalysisResultsPanel.h"
#include "widgets/ShortcutEditorDialog.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtGui/QAction>
#include <QtGui/QActionGroup>
#include <QtGui/QFontDatabase>
#include <QtGui/QIcon>
#include <QtGui/QIntValidator>
#include <QtGui/QKeySequence>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStatusBar>
#include <QtWidgets/QStyle>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace vidscope::widgets {
namespace {

constexpr qint64 kNanosecondsPerMillisecond = 1'000'000;
constexpr qint64 kNanosecondsPerSecond = 1'000'000'000;
constexpr int kMaximumFrameStepCount = 1'000;
constexpr int kMaximumFilmstripCount = static_cast<int>(
    filmstrip::FilmstripModel::kMaximumCount);

[[nodiscard]] QString pathToQString(const std::filesystem::path& path)
{
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

[[nodiscard]] std::filesystem::path qStringToPath(const QString& value)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(value.toStdWString());
#else
    return std::filesystem::path(value.toStdString());
#endif
}

[[nodiscard]] exporting::ImageFormat imageFormatForPath(
    const QString& path,
    const exporting::ImageFormat fallback = exporting::ImageFormat::Png)
{
    return exporting::ExportPlanner::formatFromPath(qStringToPath(path))
        .value_or(fallback);
}

[[nodiscard]] std::optional<exporting::ImageFormat> chooseSequenceFormat(
    QWidget* parent)
{
    const QStringList labels{
        MainWindow::tr("PNG"),
        MainWindow::tr("JPEG"),
        MainWindow::tr("WebP"),
        MainWindow::tr("BMP"),
        MainWindow::tr("TIFF"),
    };
    bool accepted = false;
    const QString selected = QInputDialog::getItem(
        parent,
        MainWindow::tr("Image Format"),
        MainWindow::tr("Export format"),
        labels,
        0,
        false,
        &accepted);
    if (!accepted) {
        return std::nullopt;
    }
    const int index = labels.indexOf(selected);
    switch (index) {
    case 1:
        return exporting::ImageFormat::Jpeg;
    case 2:
        return exporting::ImageFormat::WebP;
    case 3:
        return exporting::ImageFormat::Bmp;
    case 4:
        return exporting::ImageFormat::Tiff;
    default:
        return exporting::ImageFormat::Png;
    }
}

[[nodiscard]] QString formatTime(qint64 nanoseconds)
{
    nanoseconds = std::max<qint64>(0, nanoseconds);
    const qint64 totalMilliseconds = nanoseconds / kNanosecondsPerMillisecond;
    const qint64 milliseconds = totalMilliseconds % 1'000;
    const qint64 totalSeconds = totalMilliseconds / 1'000;
    const qint64 seconds = totalSeconds % 60;
    const qint64 totalMinutes = totalSeconds / 60;
    const qint64 minutes = totalMinutes % 60;
    const qint64 hours = totalMinutes / 60;

    return QStringLiteral("%1:%2:%3.%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(milliseconds, 3, 10, QLatin1Char('0'));
}

[[nodiscard]] QString formattedRate(AVRational rate)
{
    if (rate.num <= 0 || rate.den <= 0) {
        return QStringLiteral("unknown fps");
    }
    const double value = static_cast<double>(rate.num) / static_cast<double>(rate.den);
    return QStringLiteral("%1 fps").arg(value, 0, 'f', value < 100.0 ? 3 : 1);
}

[[nodiscard]] QString pixelFormatName(AVPixelFormat format)
{
    const char* name = av_get_pix_fmt_name(format);
    return name ? QString::fromLatin1(name) : QStringLiteral("unknown pixel format");
}

[[nodiscard]] QAction* actionByName(QObject* owner, const char* name)
{
    return owner->findChild<QAction*>(QString::fromLatin1(name));
}

[[nodiscard]] QString stateDescription(playback::PlaybackState state)
{
    switch (state) {
    case playback::PlaybackState::Closed:
        return MainWindow::tr("Ready");
    case playback::PlaybackState::Stopped:
        return MainWindow::tr("Stopped");
    case playback::PlaybackState::Paused:
        return MainWindow::tr("Paused");
    case playback::PlaybackState::Playing:
        return MainWindow::tr("Playing");
    case playback::PlaybackState::Ended:
        return MainWindow::tr("End of stream");
    case playback::PlaybackState::Error:
        return MainWindow::tr("Playback error");
    }
    return {};
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , controller_(new playback::PlaybackController({}, this))
{
    controller_->setObjectName(QStringLiteral("playbackController"));
    setObjectName(QStringLiteral("mainWindow"));
    setWindowTitle(QStringLiteral("VidScope"));
    setMinimumSize(960, 700);
    resize(1280, 860);

    createActions();
    createLayout();
    createMenus();
    createShortcuts();

    analysisManager_ = new analysis::AnalysisManager({}, this);
    exportManager_ = new exporting::ExportManager(this);
    timeline_->setAnalysisManager(analysisManager_);
    thumbnailManager_ = new thumbnails::ThumbnailManager({}, this);
    thumbnailManager_->setObjectName(QStringLiteral("thumbnailManager"));
    analysisResults_->setThumbnailManager(thumbnailManager_);
    analysisResults_->setDetectionConfig(analysisManager_->detectionConfig());
    filmstripController_ = new FilmstripController(
        timeline_,
        thumbnailManager_,
        analysisManager_,
        filmstrip_,
        {},
        this);
    hoverPreviewController_ = new HoverPreviewController(
        timeline_,
        thumbnailManager_,
        analysisManager_,
        this,
        {},
        this);

    connect(
        filmstripModeBox_,
        &QComboBox::currentIndexChanged,
        this,
        [this](const int index) {
            if (filmstripController_ == nullptr || index < 0) {
                return;
            }
            const int value = filmstripModeBox_->itemData(index).toInt();
            switch (static_cast<filmstrip::FilmstripMode>(value)) {
            case filmstrip::FilmstripMode::EntireVideo:
            case filmstrip::FilmstripMode::AroundCurrentPosition:
            case filmstrip::FilmstripMode::VisibleTimeline:
            case filmstrip::FilmstripMode::SelectedRange:
                filmstripController_->setMode(static_cast<filmstrip::FilmstripMode>(value));
                break;
            }
        });
    connect(
        filmstripCountBox_,
        &QComboBox::currentTextChanged,
        this,
        [this](const QString& text) {
            if (filmstripController_ == nullptr) {
                return;
            }
            bool valid = false;
            const int count = text.toInt(&valid);
            if (valid && count >= 1 && count <= kMaximumFilmstripCount) {
                filmstripController_->setCount(static_cast<std::size_t>(count));
            }
        });
    connect(
        filmstrip_,
        &FilmstripWidget::seekRequested,
        controller_,
        &playback::PlaybackController::seekToNanoseconds);
    connect(
        filmstrip_,
        &FilmstripWidget::frameInspectorRequested,
        this,
        [this](const qint64 timestamp, const qint64 presentationIndex) {
            controller_->pause();
            controller_->seekToNanoseconds(timestamp);
            frameInspectorDock_->show();
            frameInspectorDock_->raise();
            statusBar()->showMessage(
                presentationIndex >= 0
                    ? tr("Opening frame %1 in the Frame Inspector.")
                          .arg(presentationIndex)
                    : tr("Opening the selected frame in the Frame Inspector."),
                4000);
        });

    QSettings initialSettings;
    timeline::HeatmapMode savedHeatmapMode = timeline::HeatmapMode::Combined;
    switch (static_cast<timeline::HeatmapMode>(
        initialSettings.value(
            QStringLiteral("analysis/heatmapMode"),
            static_cast<int>(timeline::HeatmapMode::Combined)).toInt())) {
    case timeline::HeatmapMode::Motion:
        savedHeatmapMode = timeline::HeatmapMode::Motion;
        actionByName(this, "actionHeatmapMotion")->setChecked(true);
        break;
    case timeline::HeatmapMode::Similarity:
        savedHeatmapMode = timeline::HeatmapMode::Similarity;
        actionByName(this, "actionHeatmapSimilarity")->setChecked(true);
        break;
    case timeline::HeatmapMode::SceneChange:
        savedHeatmapMode = timeline::HeatmapMode::SceneChange;
        actionByName(this, "actionHeatmapSceneChange")->setChecked(true);
        break;
    case timeline::HeatmapMode::Combined:
        actionByName(this, "actionHeatmapCombined")->setChecked(true);
        break;
    }
    timeline_->setHeatmapMode(savedHeatmapMode);

    const int savedMode = initialSettings.value(
        QStringLiteral("filmstrip/mode"),
        static_cast<int>(filmstrip::FilmstripMode::EntireVideo)).toInt();
    for (int index = 0; index < filmstripModeBox_->count(); ++index) {
        if (filmstripModeBox_->itemData(index).toInt() == savedMode) {
            filmstripModeBox_->setCurrentIndex(index);
            break;
        }
    }
    const int savedCount = std::clamp(
        initialSettings.value(
            QStringLiteral("filmstrip/count"),
            static_cast<int>(filmstrip::FilmstripModel::kDefaultCount)).toInt(),
        1,
        kMaximumFilmstripCount);
    filmstripCountBox_->setCurrentText(QString::number(savedCount));
    filmstripController_->setCount(static_cast<std::size_t>(savedCount));

    connect(controller_, &playback::PlaybackController::mediaOpened,
            this, &MainWindow::handleMediaOpened);
    connect(controller_, &playback::PlaybackController::frameReady,
            this, &MainWindow::handleFrame);
    connect(controller_, &playback::PlaybackController::stateChanged,
            this, &MainWindow::handleState);
    connect(controller_, &playback::PlaybackController::errorOccurred,
            this, &MainWindow::showPlaybackError);
    connect(controller_, &playback::PlaybackController::mediaClosed, this, [this] {
        hoverPreviewController_->clear();
        filmstripController_->clear();
        exportManager_->clearMedia();
        analysisManager_->clearMedia();
        thumbnailManager_->clearMedia();
        viewport_->clearFrame();
        frameInspector_->clear();
        currentFrame_.reset();
        mediaInfo_.reset();
        timeline_->setDuration(0);
        mediaStatus_->setText(tr("No media loaded"));
        frameStatus_->setText(tr("Frame -"));
        updateSelectionStatus();
        setWindowTitle(QStringLiteral("VidScope"));
        if (auto* position = findChild<QLabel*>(QStringLiteral("positionLabel"))) {
            position->setProperty("positionNs", QVariant::fromValue<qint64>(0));
            position->setProperty("durationNs", QVariant::fromValue<qint64>(0));
            position->setText(QStringLiteral("00:00:00.000 / 00:00:00.000"));
        }
    });
    connect(controller_, &playback::PlaybackController::durationChanged, this, [this](qint64 duration) {
        timeline_->setDuration(duration);
        if (auto* position = findChild<QLabel*>(QStringLiteral("positionLabel"))) {
            position->setProperty("durationNs", QVariant::fromValue(duration));
            const auto current = position->property("positionNs").toLongLong();
            position->setText(formatTime(current) + QStringLiteral(" / ") + formatTime(duration));
        }
    });
    connect(controller_, &playback::PlaybackController::positionChanged, this, [this](qint64 positionNs) {
        timeline_->setPosition(positionNs);
        filmstripController_->setPlayhead(positionNs);
        analysisManager_->requestPlayhead(positionNs);
        if (auto* position = findChild<QLabel*>(QStringLiteral("positionLabel"))) {
            position->setProperty("positionNs", QVariant::fromValue(positionNs));
            const auto duration = position->property("durationNs").toLongLong();
            position->setText(formatTime(positionNs) + QStringLiteral(" / ") + formatTime(duration));
        }
    });
    connect(controller_, &playback::PlaybackController::metricsUpdated, this,
            [this](double decodeFps, qint64 seekMicroseconds, qsizetype cachedFrames) {
                if (auto* metrics = findChild<QLabel*>(QStringLiteral("metricsLabel"))) {
                    const QString seekText = seekMicroseconds > 0
                        ? tr(" | seek %1 ms").arg(
                              static_cast<double>(seekMicroseconds) / 1'000.0,
                              0,
                              'f',
                              1)
                        : QString{};
                    metrics->setText(
                        tr("decode %1 fps%2 | cache %3")
                            .arg(decodeFps, 0, 'f', 1)
                            .arg(seekText)
                            .arg(cachedFrames));
                }
            });
    connect(
        thumbnailManager_,
        &thumbnails::ThumbnailManager::cacheStatsChanged,
        this,
        [this](quint64 memoryHits, quint64 diskHits, quint64 misses, qsizetype memoryEntries) {
            if (auto* metrics = findChild<QLabel*>(QStringLiteral("metricsLabel"))) {
                metrics->setToolTip(
                    tr("Thumbnail cache: %1 memory hits, %2 disk hits, %3 misses, %4 memory entries")
                        .arg(memoryHits)
                        .arg(diskHits)
                        .arg(misses)
                        .arg(memoryEntries));
            }
        });
    connect(timeline_, &timeline::TimelineWidget::seekRequested,
            controller_, &playback::PlaybackController::seekToNanoseconds);
    connect(
        timeline_,
        &timeline::TimelineWidget::viewportChanged,
        analysisManager_,
        &analysis::AnalysisManager::requestVisibleRange);
    connect(timeline_, &timeline::TimelineWidget::scrubbingChanged, this, [this](bool active) {
        analysisManager_->setInteractiveActivity(active);
        statusBar()->showMessage(active ? tr("Scrubbing") : stateDescription(controller_->state()));
    });
    connect(timeline_, &timeline::TimelineWidget::selectionChanged,
            this, [this](qint64, qint64, bool) { updateSelectionStatus(); });
    connect(timeline_, &timeline::TimelineWidget::markerActivated,
            this, [this](quint64, qint64 time) {
                statusBar()->showMessage(
                    tr("Marker at %1").arg(formatTime(time)),
                    1500);
            });
    connect(
        analysisManager_,
        &analysis::AnalysisManager::samplesAvailable,
        this,
        [this](qint64 start, qint64 end, quint64) {
            updateExportActions();
            if (!currentFrame_) {
                return;
            }
            const qint64 time = static_cast<qint64>(
                currentFrame_->presentationTime.count());
            if (time < start || time > end) {
                return;
            }
            frameInspector_->setAnalysis(analysisManager_->sampleFor(
                time,
                currentFrame_->id.presentationIndex));
        });
    connect(
        analysisManager_,
        &analysis::AnalysisManager::detectionsChanged,
        this,
        [this](quint64 scenes, quint64 duplicates, quint64 freezes, quint64 samples) {
            applyDetectionResults();
            analysisStatus_->setToolTip(
                tr("%1 analyzed samples | %2 scenes | %3 duplicate ranges | %4 freezes")
                    .arg(samples)
                    .arg(scenes)
                    .arg(duplicates)
                    .arg(freezes));
        });
    connect(
        frameInspector_,
        &FrameInspectorPanel::previousFrameRequested,
        controller_,
        &playback::PlaybackController::previousFrame);
    connect(
        frameInspector_,
        &FrameInspectorPanel::nextFrameRequested,
        controller_,
        &playback::PlaybackController::nextFrame);
    connect(
        frameInspector_,
        &FrameInspectorPanel::imageZoomChanged,
        viewport_,
        &render::VideoViewport::setImageZoom);
    connect(
        frameInspector_,
        &FrameInspectorPanel::pixelInspectionChanged,
        viewport_,
        &render::VideoViewport::setPixelInspectionEnabled);
    connect(
        viewport_,
        &render::VideoViewport::pixelInspected,
        frameInspector_,
        &FrameInspectorPanel::updatePixel);
    connect(
        viewport_,
        &render::VideoViewport::pixelInspectionLeft,
        frameInspector_,
        &FrameInspectorPanel::clearPixel);
    connect(
        frameInspector_,
        &FrameInspectorPanel::comparisonDisplayChanged,
        this,
        [this](
            const QImage& frameA,
            const QImage& frameB,
            const inspection::ComparisonMode mode,
            const QImage& visualization,
            const QString& detail) {
            viewport_->setComparison(frameA, frameB, mode, visualization, detail);
        });
    connect(
        frameInspector_,
        &FrameInspectorPanel::comparisonCleared,
        viewport_,
        &render::VideoViewport::clearComparison);
    connect(
        analysisResults_,
        &AnalysisResultsPanel::seekRequested,
        controller_,
        &playback::PlaybackController::seekToNanoseconds);
    connect(
        analysisResults_,
        &AnalysisResultsPanel::reanalyzeRequested,
        analysisManager_,
        &analysis::AnalysisManager::setDetectionConfig);
    connect(
        analysisManager_,
        &analysis::AnalysisManager::progressChanged,
        this,
        [this](const double progress, const quint64 samples) {
            analysisStatus_->setText(
                tr("Analysis %1% | %2 frames")
                    .arg(progress * 100.0, 0, 'f', 0)
                    .arg(samples));
        });
    connect(
        analysisManager_,
        &analysis::AnalysisManager::stateChanged,
        this,
        [this](const analysis::AnalysisState state) {
            switch (state) {
            case analysis::AnalysisState::Idle:
                analysisStatus_->setText(tr("Analysis -"));
                break;
            case analysis::AnalysisState::LoadingCache:
                analysisStatus_->setText(tr("Analysis cache…"));
                break;
            case analysis::AnalysisState::Analyzing:
                break;
            case analysis::AnalysisState::Paused:
                analysisStatus_->setText(tr("Analysis paused"));
                break;
            case analysis::AnalysisState::Complete:
                analysisStatus_->setText(
                    tr("Analysis complete | %1 frames").arg(analysisManager_->sampleCount()));
                break;
            case analysis::AnalysisState::Error:
                analysisStatus_->setText(tr("Analysis error"));
                break;
            }
        });
    connect(
        analysisManager_,
        &analysis::AnalysisManager::errorOccurred,
        this,
        [this](const QString& detail) {
            analysisStatus_->setText(tr("Analysis error"));
            analysisStatus_->setToolTip(detail);
        });
    connect(
        exportManager_,
        &exporting::ExportManager::progressChanged,
        this,
        [this](
            quint64,
            const quint64 completed,
            const quint64 total,
            const QString& detail) {
            if (exportProgress_ == nullptr) {
                return;
            }
            exportProgress_->setLabelText(detail);
            if (total == 0) {
                exportProgress_->setRange(0, 0);
                return;
            }
            const int maximum = static_cast<int>(std::min<quint64>(
                std::numeric_limits<int>::max(),
                std::max(total, completed)));
            exportProgress_->setRange(0, maximum);
            exportProgress_->setValue(static_cast<int>(std::min<quint64>(
                static_cast<quint64>(maximum),
                completed)));
        });
    connect(
        exportManager_,
        &exporting::ExportManager::exportFinished,
        this,
        &MainWindow::finishExport);
    connect(
        exportManager_,
        &exporting::ExportManager::stateChanged,
        this,
        [this](exporting::ExportState) { updateExportActions(); });

    handleState(playback::PlaybackState::Closed);

    QSettings settings;
    const auto geometry = settings.value(QStringLiteral("mainWindow/geometry")).toByteArray();
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
}

MainWindow::~MainWindow()
{
    QSettings settings;
    settings.setValue(QStringLiteral("mainWindow/geometry"), saveGeometry());
    if (filmstripController_ != nullptr) {
        settings.setValue(
            QStringLiteral("filmstrip/mode"),
            static_cast<int>(filmstripController_->mode()));
        settings.setValue(
            QStringLiteral("filmstrip/count"),
            static_cast<qulonglong>(filmstripController_->count()));
    }
    if (timeline_ != nullptr) {
        settings.setValue(
            QStringLiteral("analysis/heatmapMode"),
            static_cast<int>(timeline_->heatmapMode()));
    }

    // Join all decode workers while their GUI receivers are still alive.
    if (exportManager_ != nullptr) {
        exportManager_->clearMedia();
    }
    if (frameInspector_ != nullptr) {
        frameInspector_->clear();
    }
    delete exportManager_;
    exportManager_ = nullptr;
    delete hoverPreviewController_;
    hoverPreviewController_ = nullptr;
    delete filmstripController_;
    filmstripController_ = nullptr;
    delete analysisManager_;
    analysisManager_ = nullptr;
    delete thumbnailManager_;
    thumbnailManager_ = nullptr;
    delete controller_;
    controller_ = nullptr;
}

void MainWindow::createActions()
{
    auto createAction = [this](const char* objectName, const QString& text) {
        auto* action = new QAction(text, this);
        action->setObjectName(QString::fromLatin1(objectName));
        action->setShortcutContext(Qt::WindowShortcut);
        addAction(action);
        return action;
    };

    auto* open = createAction("actionOpen", tr("&Open Video..."));
    open->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    connect(open, &QAction::triggered, this, &MainWindow::openFile);

    auto* playPause = createAction("actionPlayPause", tr("&Play"));
    playPause->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    connect(playPause, &QAction::triggered,
            controller_, &playback::PlaybackController::togglePlayPause);

    auto* pause = createAction("actionPause", tr("Pa&use"));
    connect(pause, &QAction::triggered, controller_, &playback::PlaybackController::pause);

    auto* stop = createAction("actionStop", tr("&Stop"));
    stop->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    connect(stop, &QAction::triggered, controller_, &playback::PlaybackController::stop);

    auto* previousFrame = createAction("actionPreviousFrame", tr("Previous &Frame"));
    previousFrame->setIcon(style()->standardIcon(QStyle::SP_MediaSkipBackward));
    connect(previousFrame, &QAction::triggered,
            controller_, &playback::PlaybackController::previousFrame);

    auto* nextFrame = createAction("actionNextFrame", tr("&Next Frame"));
    nextFrame->setIcon(style()->standardIcon(QStyle::SP_MediaSkipForward));
    connect(nextFrame, &QAction::triggered,
            controller_, &playback::PlaybackController::nextFrame);

    auto* jumpBack = createAction("actionJumpBack", tr("Jump &Back"));
    jumpBack->setIcon(style()->standardIcon(QStyle::SP_MediaSeekBackward));
    jumpBack->setToolTip(tr("Jump backward by the selected frame count (Shift+Left)"));
    connect(jumpBack, &QAction::triggered, this, [this] {
        controller_->stepFrames(-frameStepCount());
    });

    auto* jumpForward = createAction("actionJumpForward", tr("Jump &Forward"));
    jumpForward->setIcon(style()->standardIcon(QStyle::SP_MediaSeekForward));
    jumpForward->setToolTip(tr("Jump forward by the selected frame count (Shift+Right)"));
    connect(jumpForward, &QAction::triggered, this, [this] {
        controller_->stepFrames(frameStepCount());
    });

    auto* previousKeyframe = createAction("actionPreviousKeyframe", tr("Previous &Keyframe"));
    previousKeyframe->setIcon(style()->standardIcon(QStyle::SP_MediaSeekBackward));
    connect(previousKeyframe, &QAction::triggered,
            controller_, &playback::PlaybackController::previousKeyframe);

    auto* nextKeyframe = createAction("actionNextKeyframe", tr("Next K&eyframe"));
    nextKeyframe->setIcon(style()->standardIcon(QStyle::SP_MediaSeekForward));
    connect(nextKeyframe, &QAction::triggered,
            controller_, &playback::PlaybackController::nextKeyframe);

    auto* previousScene = createAction("actionPreviousScene", tr("Previous &Scene"));
    connect(previousScene, &QAction::triggered, this, [this] { seekAdjacentScene(false); });

    auto* nextScene = createAction("actionNextScene", tr("Next S&cene"));
    connect(nextScene, &QAction::triggered, this, [this] { seekAdjacentScene(true); });

    auto* zoomIn = createAction("actionTimelineZoomIn", tr("Zoom &In"));
    connect(zoomIn, &QAction::triggered, this, [this] {
        if (timeline_) {
            timeline_->zoomIn();
        }
    });

    auto* zoomOut = createAction("actionTimelineZoomOut", tr("Zoom &Out"));
    connect(zoomOut, &QAction::triggered, this, [this] {
        if (timeline_) {
            timeline_->zoomOut();
        }
    });

    auto* showAll = createAction("actionTimelineShowAll", tr("Show &Entire Video"));
    connect(showAll, &QAction::triggered, this, [this] {
        if (timeline_) {
            timeline_->showEntireMedia();
        }
    });

    auto* setIn = createAction("actionSetIn", tr("Set &In"));
    connect(setIn, &QAction::triggered, this, [this] {
        if (timeline_) {
            timeline_->setInPointAtPlayhead();
        }
    });

    auto* setOut = createAction("actionSetOut", tr("Set &Out"));
    connect(setOut, &QAction::triggered, this, [this] {
        if (timeline_) {
            timeline_->setOutPointAtPlayhead();
        }
    });

    auto* clearSelection = createAction("actionClearSelection", tr("&Clear Selection"));
    connect(clearSelection, &QAction::triggered, this, [this] {
        if (timeline_) {
            timeline_->clearSelection();
        }
    });

    auto* addMarker = createAction("actionAddMarker", tr("Add &Bookmark"));
    connect(addMarker, &QAction::triggered, this, [this] {
        if (timeline_) {
            timeline_->toggleBookmarkAtPlayhead();
        }
    });

    auto* refreshFilmstrip = createAction("actionRefreshFilmstrip", tr("&Refresh Filmstrip"));
    refreshFilmstrip->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    connect(refreshFilmstrip, &QAction::triggered, this, [this] {
        if (filmstripController_ != nullptr) {
            filmstripController_->refreshNow();
        }
    });

    auto* setFrameA = createAction("actionSetFrameA", tr("Set Frame &A"));
    connect(setFrameA, &QAction::triggered, this, [this] {
        if (frameInspector_ != nullptr) {
            frameInspector_->setCurrentAsFrameA();
        }
    });
    auto* setFrameB = createAction("actionSetFrameB", tr("Set Frame &B"));
    connect(setFrameB, &QAction::triggered, this, [this] {
        if (frameInspector_ != nullptr) {
            frameInspector_->setCurrentAsFrameB();
        }
    });
    auto* clearComparison =
        createAction("actionClearFrameComparison", tr("&Clear A/B Comparison"));
    connect(clearComparison, &QAction::triggered, this, [this] {
        if (frameInspector_ != nullptr) {
            frameInspector_->clearComparison();
        }
    });

    auto* exportCurrent =
        createAction("actionExportCurrentFrame", tr("Save &Current Frame..."));
    connect(exportCurrent, &QAction::triggered, this, [this] {
        exportSingleFrame(exporting::RelativeFrame::Current);
    });
    auto* exportPrevious =
        createAction("actionExportPreviousFrame", tr("Save &Previous Frame..."));
    connect(exportPrevious, &QAction::triggered, this, [this] {
        exportSingleFrame(exporting::RelativeFrame::Previous);
    });
    auto* exportNext =
        createAction("actionExportNextFrame", tr("Save &Next Frame..."));
    connect(exportNext, &QAction::triggered, this, [this] {
        exportSingleFrame(exporting::RelativeFrame::Next);
    });
    auto* exportSelected =
        createAction("actionExportSelectedFrames", tr("Export &Selected Frames..."));
    connect(
        exportSelected,
        &QAction::triggered,
        this,
        &MainWindow::exportSelectedFrames);
    auto* exportEveryN =
        createAction("actionExportEveryNFrames", tr("Export Every &N Frames..."));
    connect(
        exportEveryN,
        &QAction::triggered,
        this,
        &MainWindow::exportEveryNFrames);
    auto* exportKeyframes =
        createAction("actionExportKeyframes", tr("Export &Keyframes..."));
    connect(
        exportKeyframes,
        &QAction::triggered,
        this,
        &MainWindow::exportKeyframes);
    auto* exportScenes =
        createAction("actionExportSceneFrames", tr("Export Scene &Frames..."));
    connect(
        exportScenes,
        &QAction::triggered,
        this,
        &MainWindow::exportSceneFrames);
    auto* exportHighMotion =
        createAction("actionExportHighMotionFrames", tr("Export &High-Motion Frames..."));
    connect(
        exportHighMotion,
        &QAction::triggered,
        this,
        &MainWindow::exportHighMotionFrames);
    auto* contactSheet =
        createAction("actionCreateContactSheet", tr("Create Contact &Sheet..."));
    connect(
        contactSheet,
        &QAction::triggered,
        this,
        &MainWindow::createContactSheet);
    auto* cancelExport =
        createAction("actionCancelExport", tr("Cancel E&xport"));
    connect(cancelExport, &QAction::triggered, this, [this] {
        if (exportManager_ != nullptr) {
            exportManager_->cancel();
        }
    });

    auto* heatmapModes = new QActionGroup(this);
    heatmapModes->setObjectName(QStringLiteral("heatmapModeGroup"));
    heatmapModes->setExclusive(true);
    const auto createHeatmapMode = [&](const char* name, const QString& text, const auto mode) {
        auto* action = createAction(name, text);
        action->setCheckable(true);
        heatmapModes->addAction(action);
        connect(action, &QAction::triggered, this, [this, mode] {
            if (timeline_ != nullptr) {
                timeline_->setHeatmapMode(mode);
            }
        });
        return action;
    };
    (void)createHeatmapMode(
        "actionHeatmapMotion",
        tr("&Motion"),
        timeline::HeatmapMode::Motion);
    (void)createHeatmapMode(
        "actionHeatmapSimilarity",
        tr("&Similarity"),
        timeline::HeatmapMode::Similarity);
    (void)createHeatmapMode(
        "actionHeatmapSceneChange",
        tr("Scene &Change"),
        timeline::HeatmapMode::SceneChange);
    auto* combinedHeatmap = createHeatmapMode(
        "actionHeatmapCombined",
        tr("&Combined"),
        timeline::HeatmapMode::Combined);
    combinedHeatmap->setChecked(true);

    auto* fullscreen = createAction("actionFullscreen", tr("&Full Screen"));
    fullscreen->setCheckable(true);
    connect(fullscreen, &QAction::toggled, this, [this](bool enabled) {
        enabled ? showFullScreen() : showNormal();
    });

    auto* shortcuts = createAction("actionKeyboardShortcuts", tr("&Keyboard Shortcuts..."));
    connect(shortcuts, &QAction::triggered, this, &MainWindow::showShortcutEditor);

    auto* exit = createAction("actionExit", tr("E&xit"));
    connect(exit, &QAction::triggered, this, &QWidget::close);

    auto* about = createAction("actionAbout", tr("&About VidScope"));
    connect(about, &QAction::triggered, this, [this] {
        const QString applicationVersion = QApplication::applicationVersion().toHtmlEscaped();
        const QString productName = applicationVersion.isEmpty()
            ? tr("VidScope")
            : tr("VidScope %1").arg(applicationVersion);
        QMessageBox::about(
            this,
            tr("About VidScope"),
            tr("<b>%1</b><br>Frame-accurate video inspection built with Qt and FFmpeg.")
                .arg(productName));
    });
}

void MainWindow::createLayout()
{
    auto* central = new QWidget(this);
    central->setObjectName(QStringLiteral("centralSurface"));
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(8);

    viewport_ = new render::VideoViewport(central);
    root->addWidget(viewport_, 1);

    auto* controls = new QFrame(central);
    controls->setObjectName(QStringLiteral("controlSurface"));
    auto* controlsLayout = new QVBoxLayout(controls);
    controlsLayout->setContentsMargins(14, 10, 14, 10);
    controlsLayout->setSpacing(7);

    timeline_ = new timeline::TimelineWidget(controls);
    controlsLayout->addWidget(timeline_);

    auto* filmstripHeader = new QHBoxLayout;
    filmstripHeader->setContentsMargins(0, 0, 0, 0);
    filmstripHeader->setSpacing(7);

    auto* filmstripTitle = new QLabel(tr("Preview Filmstrip"), controls);
    filmstripTitle->setObjectName(QStringLiteral("filmstripTitle"));
    filmstripHeader->addWidget(filmstripTitle);
    filmstripHeader->addStretch(1);

    auto* modeLabel = new QLabel(tr("Mode"), controls);
    filmstripHeader->addWidget(modeLabel);
    filmstripModeBox_ = new QComboBox(controls);
    filmstripModeBox_->setObjectName(QStringLiteral("filmstripMode"));
    filmstripModeBox_->addItem(
        tr("Entire Video"),
        static_cast<int>(filmstrip::FilmstripMode::EntireVideo));
    filmstripModeBox_->addItem(
        tr("Around Current Position"),
        static_cast<int>(filmstrip::FilmstripMode::AroundCurrentPosition));
    filmstripModeBox_->addItem(
        tr("Visible Timeline"),
        static_cast<int>(filmstrip::FilmstripMode::VisibleTimeline));
    filmstripModeBox_->addItem(
        tr("Selected Range"),
        static_cast<int>(filmstrip::FilmstripMode::SelectedRange));
    filmstripModeBox_->setAccessibleName(tr("Filmstrip range mode"));
    filmstripHeader->addWidget(filmstripModeBox_);

    auto* countLabel = new QLabel(tr("Frames"), controls);
    filmstripHeader->addWidget(countLabel);
    filmstripCountBox_ = new QComboBox(controls);
    filmstripCountBox_->setObjectName(QStringLiteral("filmstripCount"));
    filmstripCountBox_->setEditable(true);
    filmstripCountBox_->setInsertPolicy(QComboBox::NoInsert);
    filmstripCountBox_->setValidator(new QIntValidator(1, kMaximumFilmstripCount, filmstripCountBox_));
    filmstripCountBox_->addItems({
        QStringLiteral("8"),
        QStringLiteral("16"),
        QStringLiteral("20"),
        QStringLiteral("32")});
    filmstripCountBox_->setCurrentText(
        QString::number(filmstrip::FilmstripModel::kDefaultCount));
    filmstripCountBox_->setFixedWidth(68);
    filmstripCountBox_->setAccessibleName(tr("Filmstrip frame count"));
    filmstripCountBox_->setToolTip(
        tr("Preview count (presets: 8, 16, 20, 32; custom maximum %1)")
            .arg(kMaximumFilmstripCount));
    filmstripHeader->addWidget(filmstripCountBox_);

    auto* refreshFilmstripButton = new QToolButton(controls);
    refreshFilmstripButton->setDefaultAction(actionByName(this, "actionRefreshFilmstrip"));
    refreshFilmstripButton->setAccessibleName(tr("Refresh filmstrip"));
    refreshFilmstripButton->setAutoRaise(true);
    refreshFilmstripButton->setFixedSize(30, 28);
    filmstripHeader->addWidget(refreshFilmstripButton);
    controlsLayout->addLayout(filmstripHeader);

    auto* filmstripScrollArea = new QScrollArea(controls);
    filmstripScrollArea->setObjectName(QStringLiteral("filmstripScrollArea"));
    filmstripScrollArea->setFrameShape(QFrame::NoFrame);
    filmstripScrollArea->setWidgetResizable(true);
    filmstripScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    filmstripScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    filmstripScrollArea->setMinimumHeight(142);
    filmstripScrollArea->setMaximumHeight(172);
    filmstrip_ = new FilmstripWidget(filmstripScrollArea);
    filmstripScrollArea->setWidget(filmstrip_);
    controlsLayout->addWidget(filmstripScrollArea);

    auto* transport = new QHBoxLayout;
    transport->setContentsMargins(0, 0, 0, 0);
    transport->setSpacing(6);

    mediaStatus_ = new QLabel(tr("No media loaded"), controls);
    mediaStatus_->setObjectName(QStringLiteral("mediaStatus"));
    mediaStatus_->setMinimumWidth(200);
    mediaStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    transport->addWidget(mediaStatus_, 1);

    auto makeButton = [controls](QAction* action, const QString& accessibleName) {
        auto* button = new QToolButton(controls);
        button->setDefaultAction(action);
        button->setAccessibleName(accessibleName);
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setFixedSize(36, 32);
        button->setIconSize(QSize(18, 18));
        return button;
    };

    transport->addWidget(makeButton(
        actionByName(this, "actionPreviousKeyframe"), tr("Previous keyframe")));
    transport->addWidget(makeButton(
        actionByName(this, "actionJumpBack"), tr("Jump backward by selected frame count")));

    frameStepBox_ = new QComboBox(controls);
    frameStepBox_->setObjectName(QStringLiteral("frameStepCount"));
    frameStepBox_->setEditable(true);
    frameStepBox_->setInsertPolicy(QComboBox::NoInsert);
    frameStepBox_->setValidator(new QIntValidator(1, kMaximumFrameStepCount, frameStepBox_));
    frameStepBox_->addItems({
        QStringLiteral("1"),
        QStringLiteral("2"),
        QStringLiteral("5"),
        QStringLiteral("10")});
    frameStepBox_->setCurrentText(QStringLiteral("10"));
    frameStepBox_->setFixedWidth(58);
    frameStepBox_->setAccessibleName(tr("Frames per jump"));
    frameStepBox_->setToolTip(tr("Frames per jump (presets: 1, 2, 5, 10; maximum %1)")
                                  .arg(kMaximumFrameStepCount));
    transport->addWidget(frameStepBox_);
    transport->addWidget(makeButton(
        actionByName(this, "actionJumpForward"), tr("Jump forward by selected frame count")));

    transport->addWidget(makeButton(
        actionByName(this, "actionPreviousFrame"), tr("Previous frame")));
    playPauseButton_ = makeButton(actionByName(this, "actionPlayPause"), tr("Play or pause"));
    playPauseButton_->setObjectName(QStringLiteral("playPauseButton"));
    transport->addWidget(playPauseButton_);
    transport->addWidget(makeButton(actionByName(this, "actionStop"), tr("Stop")));
    transport->addWidget(makeButton(actionByName(this, "actionNextFrame"), tr("Next frame")));
    transport->addWidget(makeButton(
        actionByName(this, "actionNextKeyframe"), tr("Next keyframe")));

    auto* position = new QLabel(QStringLiteral("00:00:00.000 / 00:00:00.000"), controls);
    position->setObjectName(QStringLiteral("positionLabel"));
    position->setProperty("positionNs", QVariant::fromValue<qint64>(0));
    position->setProperty("durationNs", QVariant::fromValue<qint64>(0));
    position->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    position->setMinimumWidth(220);
    position->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    transport->addWidget(position, 1);
    controlsLayout->addLayout(transport);

    auto* inspection = new QHBoxLayout;
    inspection->setContentsMargins(0, 0, 0, 0);
    inspection->setSpacing(12);

    frameStatus_ = new QLabel(tr("Frame -"), controls);
    frameStatus_->setObjectName(QStringLiteral("frameStatus"));
    frameStatus_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    frameStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    inspection->addWidget(frameStatus_, 2);

    selectionStatus_ = new QLabel(tr("Selection -"), controls);
    selectionStatus_->setObjectName(QStringLiteral("selectionStatus"));
    selectionStatus_->setAlignment(Qt::AlignCenter);
    selectionStatus_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    inspection->addWidget(selectionStatus_, 1);

    analysisStatus_ = new QLabel(tr("Analysis -"), controls);
    analysisStatus_->setObjectName(QStringLiteral("analysisStatus"));
    analysisStatus_->setAlignment(Qt::AlignCenter);
    inspection->addWidget(analysisStatus_);

    auto* metrics = new QLabel(tr("decode - | cache 0"), controls);
    metrics->setObjectName(QStringLiteral("metricsLabel"));
    metrics->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    inspection->addWidget(metrics);
    controlsLayout->addLayout(inspection);

    root->addWidget(controls);
    setCentralWidget(central);

    analysisResultsDock_ = new QDockWidget(tr("Analysis Results"), this);
    analysisResultsDock_->setObjectName(QStringLiteral("analysisResultsDock"));
    analysisResultsDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    analysisResultsDock_->setMinimumWidth(340);
    analysisResults_ = new AnalysisResultsPanel(analysisResultsDock_);
    analysisResultsDock_->setWidget(analysisResults_);
    addDockWidget(Qt::RightDockWidgetArea, analysisResultsDock_);

    frameInspectorDock_ = new QDockWidget(tr("Frame Inspector"), this);
    frameInspectorDock_->setObjectName(QStringLiteral("frameInspectorDock"));
    frameInspectorDock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    frameInspectorDock_->setMinimumWidth(380);
    frameInspector_ = new FrameInspectorPanel(frameInspectorDock_);
    frameInspectorDock_->setWidget(frameInspector_);
    addDockWidget(Qt::RightDockWidgetArea, frameInspectorDock_);
    tabifyDockWidget(analysisResultsDock_, frameInspectorDock_);
    frameInspectorDock_->raise();

    statusBar()->setSizeGripEnabled(true);
    statusBar()->showMessage(tr("Ready"));

    setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget#centralSurface {
            background: #111419;
            color: #d9dee7;
        }
        QMenuBar {
            background: #171b21;
            color: #d9dee7;
            border-bottom: 1px solid #282e37;
        }
        QMenuBar::item { padding: 6px 10px; }
        QMenuBar::item:selected, QMenu::item:selected { background: #2a3442; }
        QMenu {
            background: #1b2027;
            color: #d9dee7;
            border: 1px solid #343b46;
        }
        QMenu::item { padding: 6px 28px 6px 24px; }
        QFrame#controlSurface {
            background: #191d24;
            border: 1px solid #2d333d;
            border-radius: 6px;
        }
        QLabel#mediaStatus { color: #c5ccd7; }
        QLabel#frameStatus { color: #b8c2d0; }
        QLabel#selectionStatus { color: #7db7e8; }
        QLabel#analysisStatus { color: #8bc9a7; }
        QLabel#metricsLabel { color: #7f8a99; }
        QLabel#positionLabel { color: #dce7f5; }
        QLabel#filmstripTitle { color: #dce7f5; font-weight: 600; }
        QScrollArea#filmstripScrollArea { background: #0f1217; border: 1px solid #282f39; }
        QComboBox {
            background: #202630;
            color: #d9dee7;
            border: 1px solid #394351;
            border-radius: 4px;
            padding: 3px 7px;
        }
        QComboBox:disabled { color: #636d7a; background: #191d23; }
        QToolButton {
            background: transparent;
            border: 1px solid transparent;
            border-radius: 5px;
            padding: 4px;
        }
        QToolButton:hover { background: #28313d; border-color: #3b4655; }
        QToolButton:pressed { background: #1e6fba; }
        QToolButton:disabled { color: #5d6570; }
        QStatusBar {
            background: #171b21;
            color: #8f99a7;
            border-top: 1px solid #282e37;
        }
    )"));
}

void MainWindow::createMenus()
{
    auto* fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(actionByName(this, "actionOpen"));
    fileMenu->addSeparator();
    fileMenu->addAction(actionByName(this, "actionExit"));

    auto* exportMenu = menuBar()->addMenu(tr("&Export"));
    exportMenu->addAction(actionByName(this, "actionExportCurrentFrame"));
    exportMenu->addAction(actionByName(this, "actionExportPreviousFrame"));
    exportMenu->addAction(actionByName(this, "actionExportNextFrame"));
    exportMenu->addSeparator();
    exportMenu->addAction(actionByName(this, "actionExportSelectedFrames"));
    exportMenu->addAction(actionByName(this, "actionExportEveryNFrames"));
    exportMenu->addAction(actionByName(this, "actionExportKeyframes"));
    exportMenu->addAction(actionByName(this, "actionExportSceneFrames"));
    exportMenu->addAction(actionByName(this, "actionExportHighMotionFrames"));
    exportMenu->addSeparator();
    exportMenu->addAction(actionByName(this, "actionCreateContactSheet"));
    exportMenu->addSeparator();
    exportMenu->addAction(actionByName(this, "actionCancelExport"));

    auto* playbackMenu = menuBar()->addMenu(tr("&Playback"));
    playbackMenu->addAction(actionByName(this, "actionPlayPause"));
    playbackMenu->addAction(actionByName(this, "actionPause"));
    playbackMenu->addAction(actionByName(this, "actionStop"));

    auto* navigationMenu = menuBar()->addMenu(tr("&Navigation"));
    navigationMenu->addAction(actionByName(this, "actionPreviousFrame"));
    navigationMenu->addAction(actionByName(this, "actionNextFrame"));
    navigationMenu->addAction(actionByName(this, "actionJumpBack"));
    navigationMenu->addAction(actionByName(this, "actionJumpForward"));
    navigationMenu->addSeparator();
    navigationMenu->addAction(actionByName(this, "actionPreviousKeyframe"));
    navigationMenu->addAction(actionByName(this, "actionNextKeyframe"));
    navigationMenu->addAction(actionByName(this, "actionPreviousScene"));
    navigationMenu->addAction(actionByName(this, "actionNextScene"));

    auto* timelineMenu = menuBar()->addMenu(tr("&Timeline"));
    timelineMenu->addAction(actionByName(this, "actionTimelineZoomIn"));
    timelineMenu->addAction(actionByName(this, "actionTimelineZoomOut"));
    timelineMenu->addAction(actionByName(this, "actionTimelineShowAll"));
    timelineMenu->addSeparator();
    timelineMenu->addAction(actionByName(this, "actionSetIn"));
    timelineMenu->addAction(actionByName(this, "actionSetOut"));
    timelineMenu->addAction(actionByName(this, "actionClearSelection"));
    timelineMenu->addSeparator();
    timelineMenu->addAction(actionByName(this, "actionAddMarker"));
    timelineMenu->addSeparator();
    timelineMenu->addAction(actionByName(this, "actionRefreshFilmstrip"));

    auto* analysisMenu = menuBar()->addMenu(tr("&Analysis"));
    analysisMenu->addAction(actionByName(this, "actionHeatmapMotion"));
    analysisMenu->addAction(actionByName(this, "actionHeatmapSimilarity"));
    analysisMenu->addAction(actionByName(this, "actionHeatmapSceneChange"));
    analysisMenu->addAction(actionByName(this, "actionHeatmapCombined"));

    auto* inspectionMenu = menuBar()->addMenu(tr("&Inspection"));
    inspectionMenu->addAction(actionByName(this, "actionSetFrameA"));
    inspectionMenu->addAction(actionByName(this, "actionSetFrameB"));
    inspectionMenu->addAction(actionByName(this, "actionClearFrameComparison"));
    inspectionMenu->addSeparator();
    inspectionMenu->addAction(actionByName(this, "actionPreviousFrame"));
    inspectionMenu->addAction(actionByName(this, "actionNextFrame"));

    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(actionByName(this, "actionFullscreen"));
    viewMenu->addAction(analysisResultsDock_->toggleViewAction());
    viewMenu->addAction(frameInspectorDock_->toggleViewAction());

    auto* settingsMenu = menuBar()->addMenu(tr("&Settings"));
    settingsMenu->addAction(actionByName(this, "actionKeyboardShortcuts"));

    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(actionByName(this, "actionAbout"));
}

void MainWindow::createShortcuts()
{
    auto configure = [this](const char* name, QList<QKeySequence> defaults) {
        ShortcutEditorDialog::configureAction(actionByName(this, name), defaults);
    };

    configure("actionOpen", {QKeySequence(QKeySequence::Open)});
    configure("actionExit", {QKeySequence(QKeySequence::Quit)});
    configure("actionPlayPause", {QKeySequence(Qt::Key_Space)});
    configure("actionPause", {QKeySequence(Qt::Key_K)});
    configure("actionStop", {QKeySequence(Qt::Key_S)});
    configure("actionPreviousFrame", {
        QKeySequence(Qt::Key_Left),
        QKeySequence(Qt::Key_Comma),
        QKeySequence(Qt::Key_J)});
    configure("actionNextFrame", {
        QKeySequence(Qt::Key_Right),
        QKeySequence(Qt::Key_Period),
        QKeySequence(Qt::Key_L)});
    configure("actionPreviousKeyframe", {
        QKeySequence(QKeyCombination(Qt::ControlModifier, Qt::Key_Left))});
    configure("actionNextKeyframe", {
        QKeySequence(QKeyCombination(Qt::ControlModifier, Qt::Key_Right))});
    configure("actionJumpBack", {
        QKeySequence(QKeyCombination(Qt::ShiftModifier, Qt::Key_Left))});
    configure("actionJumpForward", {
        QKeySequence(QKeyCombination(Qt::ShiftModifier, Qt::Key_Right))});
    configure("actionPreviousScene", {
        QKeySequence(QKeyCombination(Qt::AltModifier, Qt::Key_Left))});
    configure("actionNextScene", {
        QKeySequence(QKeyCombination(Qt::AltModifier, Qt::Key_Right))});
    configure("actionSetIn", {QKeySequence(Qt::Key_I)});
    configure("actionSetOut", {QKeySequence(Qt::Key_O)});
    configure("actionAddMarker", {QKeySequence(Qt::Key_M)});
    configure("actionClearSelection", {
        QKeySequence(QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_X))});
    configure("actionTimelineZoomIn", {QKeySequence(QKeySequence::ZoomIn)});
    configure("actionTimelineZoomOut", {QKeySequence(QKeySequence::ZoomOut)});
    configure("actionTimelineShowAll", {
        QKeySequence(QKeyCombination(Qt::ControlModifier, Qt::Key_0))});
    configure("actionFullscreen", {QKeySequence(Qt::Key_F11)});
    configure("actionSetFrameA", {
        QKeySequence(QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_A))});
    configure("actionSetFrameB", {
        QKeySequence(QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_B))});
    configure("actionExportCurrentFrame", {
        QKeySequence(QKeyCombination(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_S))});
    configure("actionCreateContactSheet", {
        QKeySequence(QKeyCombination(Qt::ControlModifier | Qt::AltModifier, Qt::Key_C))});
}

int MainWindow::frameStepCount() const
{
    if (!frameStepBox_) {
        return 1;
    }
    bool valid = false;
    const auto value = frameStepBox_->currentText().toInt(&valid);
    return valid ? std::clamp(value, 1, kMaximumFrameStepCount) : 1;
}

void MainWindow::openFile()
{
    QSettings settings;
    const QString startDirectory = settings.value(
        QStringLiteral("open/lastDirectory"),
        QDir::homePath()).toString();
    const QString path = QFileDialog::getOpenFileName(
        this,
        tr("Open Video"),
        startDirectory,
        tr("Video files (*.mp4 *.mkv *.mov *.avi *.webm *.m4v *.ts *.mts *.m2ts);;All files (*.*)"));
    if (path.isEmpty()) {
        return;
    }

    settings.setValue(QStringLiteral("open/lastDirectory"), QFileInfo(path).absolutePath());

    // Invalidate old playback and preview generations synchronously. Any
    // already-queued delivery is ignored until the new media lifecycle exists.
    hoverPreviewController_->clear();
    filmstripController_->clear();
    exportManager_->clearMedia();
    analysisManager_->clearMedia();
    thumbnailManager_->clearMedia();
    frameInspector_->clear();
    currentFrame_.reset();
    mediaInfo_.reset();
    opening_ = true;
    timeline_->setDuration(0);
    updateSelectionStatus();
    handleState(controller_->state());
    statusBar()->showMessage(tr("Opening %1...").arg(QFileInfo(path).fileName()));
    controller_->openFile(path);
}

void MainWindow::handleMediaOpened(media::MediaInfoPtr info)
{
    if (!info) {
        return;
    }

    opening_ = false;
    mediaInfo_ = info;
    exportManager_->setMedia(info);
    thumbnailManager_->setMedia(info);
    analysisManager_->setMedia(info);
    handleState(controller_->state());
    timeline_->setDuration(static_cast<qint64>(info->duration.count()));
    filmstripController_->setMedia(info);
    updateSelectionStatus();

    const QString fileName = pathToQString(info->path.filename());
    setWindowTitle(tr("%1 - VidScope").arg(fileName));

    const QString codec = !info->codecLongName.empty()
        ? QString::fromStdString(info->codecLongName)
        : QString::fromStdString(info->codecName);
    mediaStatus_->setText(
        tr("%1 | %2x%3 | %4 | %5")
            .arg(fileName)
            .arg(info->width)
            .arg(info->height)
            .arg(codec)
            .arg(formattedRate(info->averageFrameRate)));

    const QString container = !info->containerLongName.empty()
        ? QString::fromStdString(info->containerLongName)
        : QString::fromStdString(info->containerName);
    mediaStatus_->setToolTip(
        tr("Container: %1\nCodec: %2\nPixel format: %3\nBit depth: %4\n"
           "Time base: %5/%6\nDuration: %7\nDeclared frames: %8")
            .arg(container)
            .arg(codec)
            .arg(pixelFormatName(info->pixelFormat))
            .arg(info->bitDepth)
            .arg(info->timeBase.num)
            .arg(info->timeBase.den)
            .arg(formatTime(static_cast<qint64>(info->duration.count())))
            .arg(info->declaredFrameCount > 0 ? QString::number(info->declaredFrameCount)
                                              : tr("unknown")));
    statusBar()->showMessage(tr("Opened %1").arg(fileName), 4000);
}

void MainWindow::handleFrame(media::DecodedFramePtr frame, const QImage& image)
{
    if (opening_ || !frame || image.isNull()) {
        return;
    }
    timeline_->observeFrame(*frame);
    filmstripController_->setPlayhead(
        static_cast<qint64>(frame->presentationTime.count()));
    filmstripController_->notifyFrameObserved();
    currentFrame_ = frame;
    viewport_->setFrame(image);
    frameInspector_->setFrame(
        frame,
        image,
        analysisManager_->sampleFor(
            static_cast<qint64>(frame->presentationTime.count()),
            frame->id.presentationIndex));
    updateFrameStatus(*frame);
    updateSelectionStatus();
    updateExportActions();
}

void MainWindow::handleState(playback::PlaybackState state)
{
    const bool hasMedia = !opening_
        && state != playback::PlaybackState::Closed
        && state != playback::PlaybackState::Error;
    const bool playing = state == playback::PlaybackState::Playing;
    analysisManager_->setPlaybackActive(playing);

    if (auto* playPause = actionByName(this, "actionPlayPause")) {
        playPause->setEnabled(hasMedia);
        playPause->setText(playing ? tr("&Pause") : tr("&Play"));
        playPause->setToolTip(playing ? tr("Pause (Space or K)") : tr("Play (Space)"));
        playPause->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause
                                                        : QStyle::SP_MediaPlay));
    }
    for (const char* name : {
             "actionPause",
             "actionStop",
             "actionPreviousFrame",
             "actionNextFrame",
             "actionJumpBack",
             "actionJumpForward",
             "actionPreviousKeyframe",
             "actionNextKeyframe",
             "actionPreviousScene",
             "actionNextScene",
             "actionTimelineZoomIn",
             "actionTimelineZoomOut",
             "actionTimelineShowAll",
             "actionSetIn",
             "actionSetOut",
             "actionClearSelection",
             "actionAddMarker",
             "actionRefreshFilmstrip",
             "actionSetFrameA",
             "actionSetFrameB",
             "actionClearFrameComparison"}) {
        if (auto* action = actionByName(this, name)) {
            action->setEnabled(hasMedia);
        }
    }
    frameStepBox_->setEnabled(hasMedia);
    filmstripModeBox_->setEnabled(hasMedia);
    filmstripCountBox_->setEnabled(hasMedia);
    analysisResults_->setEnabled(hasMedia);
    frameInspector_->setEnabled(hasMedia);
    frameInspector_->setPaused(hasMedia && !playing);
    statusBar()->showMessage(stateDescription(state));
    updateExportActions();
}

void MainWindow::exportSingleFrame(const exporting::RelativeFrame relativeFrame)
{
    if (!mediaInfo_ || !currentFrame_) {
        return;
    }
    QSettings settings;
    const QString directory = settings.value(
        QStringLiteral("export/lastDirectory"),
        QDir::homePath()).toString();
    const QString stem = exporting::ExportPlanner::sanitizedBaseName(
        pathToQString(mediaInfo_->path.stem()));
    QString qualifier;
    switch (relativeFrame) {
    case exporting::RelativeFrame::Previous:
        qualifier = QStringLiteral("_previous");
        break;
    case exporting::RelativeFrame::Current:
        qualifier = QStringLiteral("_current");
        break;
    case exporting::RelativeFrame::Next:
        qualifier = QStringLiteral("_next");
        break;
    }
    const QString path = QFileDialog::getSaveFileName(
        this,
        tr("Save Full-Resolution Frame"),
        QDir(directory).filePath(stem + qualifier + QStringLiteral(".png")),
        exporting::ExportPlanner::fileDialogFilter());
    if (path.isEmpty()) {
        return;
    }
    settings.setValue(
        QStringLiteral("export/lastDirectory"),
        QFileInfo(path).absolutePath());

    exporting::ExportRequest request;
    request.kind = exporting::ExportKind::SingleFrame;
    request.relativeFrame = relativeFrame;
    request.anchor = currentFrame_->presentationTime;
    request.outputPath = qStringToPath(path);
    request.format = imageFormatForPath(path);
    request.overwrite = true;
    startExport(std::move(request));
}

void MainWindow::exportSelectedFrames()
{
    if (!mediaInfo_ || !timeline_->model().selection()) {
        statusBar()->showMessage(tr("Select a timeline range before exporting frames."), 3500);
        return;
    }
    const auto format = chooseSequenceFormat(this);
    if (!format) {
        return;
    }
    QSettings settings;
    const QString directory = QFileDialog::getExistingDirectory(
        this,
        tr("Export Selected Frames"),
        settings.value(
            QStringLiteral("export/lastDirectory"),
            QDir::homePath()).toString());
    if (directory.isEmpty()) {
        return;
    }
    settings.setValue(QStringLiteral("export/lastDirectory"), directory);
    const auto selection = *timeline_->model().selection();
    exporting::ExportRequest request;
    request.kind = exporting::ExportKind::FrameRange;
    request.outputPath = qStringToPath(directory);
    request.baseName = pathToQString(mediaInfo_->path.stem());
    request.format = *format;
    request.rangeStart = selection.start;
    request.rangeEnd = selection.end;
    startExport(std::move(request));
}

void MainWindow::exportEveryNFrames()
{
    if (!mediaInfo_) {
        return;
    }
    bool accepted = false;
    const int everyN = QInputDialog::getInt(
        this,
        tr("Export Every N Frames"),
        tr("Export one frame for every N decoded presentation frames"),
        10,
        1,
        1'000'000,
        1,
        &accepted);
    if (!accepted) {
        return;
    }
    const auto format = chooseSequenceFormat(this);
    if (!format) {
        return;
    }
    QSettings settings;
    const QString directory = QFileDialog::getExistingDirectory(
        this,
        tr("Export Every N Frames"),
        settings.value(
            QStringLiteral("export/lastDirectory"),
            QDir::homePath()).toString());
    if (directory.isEmpty()) {
        return;
    }
    settings.setValue(QStringLiteral("export/lastDirectory"), directory);

    exporting::ExportRequest request;
    request.kind = exporting::ExportKind::FrameRange;
    request.outputPath = qStringToPath(directory);
    request.baseName = pathToQString(mediaInfo_->path.stem());
    request.format = *format;
    request.everyNFrames = static_cast<std::size_t>(everyN);
    request.rangeStart = media::MediaTime::zero();
    request.rangeEnd = mediaInfo_->duration;
    if (const auto& selection = timeline_->model().selection()) {
        request.rangeStart = selection->start;
        request.rangeEnd = selection->end;
    }
    startExport(std::move(request));
}

void MainWindow::exportKeyframes()
{
    if (!mediaInfo_) {
        return;
    }
    const auto format = chooseSequenceFormat(this);
    if (!format) {
        return;
    }
    QSettings settings;
    const QString directory = QFileDialog::getExistingDirectory(
        this,
        tr("Export Keyframes"),
        settings.value(
            QStringLiteral("export/lastDirectory"),
            QDir::homePath()).toString());
    if (directory.isEmpty()) {
        return;
    }
    settings.setValue(QStringLiteral("export/lastDirectory"), directory);

    exporting::ExportRequest request;
    request.kind = exporting::ExportKind::Keyframes;
    request.outputPath = qStringToPath(directory);
    request.baseName = pathToQString(mediaInfo_->path.stem());
    request.format = *format;
    request.rangeEnd = mediaInfo_->duration;
    startExport(std::move(request));
}

void MainWindow::exportSceneFrames()
{
    if (!mediaInfo_ || analysisManager_ == nullptr) {
        return;
    }
    const auto detections = analysisManager_->detectionResults();
    if (detections.scenes.empty()) {
        statusBar()->showMessage(tr("No detected scene frames are available."), 3500);
        return;
    }
    const auto format = chooseSequenceFormat(this);
    if (!format) {
        return;
    }
    QSettings settings;
    const QString directory = QFileDialog::getExistingDirectory(
        this,
        tr("Export Scene Frames"),
        settings.value(
            QStringLiteral("export/lastDirectory"),
            QDir::homePath()).toString());
    if (directory.isEmpty()) {
        return;
    }
    settings.setValue(QStringLiteral("export/lastDirectory"), directory);

    exporting::ExportRequest request;
    request.kind = exporting::ExportKind::SceneFrames;
    request.outputPath = qStringToPath(directory);
    request.baseName = pathToQString(mediaInfo_->path.stem()) + QStringLiteral("_scene");
    request.format = *format;
    request.targetTimes.reserve(detections.scenes.size());
    for (const auto& scene : detections.scenes) {
        request.targetTimes.push_back(scene.start);
    }
    startExport(std::move(request));
}

void MainWindow::exportHighMotionFrames()
{
    if (!mediaInfo_ || analysisManager_ == nullptr) {
        return;
    }
    bool accepted = false;
    const double thresholdPercent = QInputDialog::getDouble(
        this,
        tr("Export High-Motion Frames"),
        tr("Minimum motion score (%)"),
        70.0,
        0.0,
        100.0,
        1,
        &accepted);
    if (!accepted) {
        return;
    }
    const auto samples = analysisManager_->samplesInRange(
        0,
        static_cast<qint64>(mediaInfo_->duration.count()),
        exporting::ExportPlanner::kMaximumFrameExports);
    std::vector<media::MediaTime> targets;
    targets.reserve(samples.size());
    const float threshold = static_cast<float>(thresholdPercent / 100.0);
    for (const auto& sample : samples) {
        if (sample.motion && *sample.motion >= threshold) {
            targets.push_back(sample.presentationTime);
        }
    }
    if (targets.empty()) {
        statusBar()->showMessage(
            tr("No analyzed frames meet the high-motion threshold."),
            3500);
        return;
    }
    const auto format = chooseSequenceFormat(this);
    if (!format) {
        return;
    }
    QSettings settings;
    const QString directory = QFileDialog::getExistingDirectory(
        this,
        tr("Export High-Motion Frames"),
        settings.value(
            QStringLiteral("export/lastDirectory"),
            QDir::homePath()).toString());
    if (directory.isEmpty()) {
        return;
    }
    settings.setValue(QStringLiteral("export/lastDirectory"), directory);

    exporting::ExportRequest request;
    request.kind = exporting::ExportKind::HighMotionFrames;
    request.outputPath = qStringToPath(directory);
    request.baseName = pathToQString(mediaInfo_->path.stem()) + QStringLiteral("_motion");
    request.format = *format;
    request.targetTimes = std::move(targets);
    startExport(std::move(request));
}

void MainWindow::createContactSheet()
{
    if (!mediaInfo_) {
        return;
    }
    QStringList sourceLabels;
    std::vector<exporting::ContactSheetSource> sources;
    const auto addSource = [&](const QString& label, const auto source) {
        sourceLabels.push_back(label);
        sources.push_back(source);
    };
    addSource(
        tr("Entire video"),
        exporting::ContactSheetSource::EntireVideo);
    addSource(
        tr("Visible timeline"),
        exporting::ContactSheetSource::VisibleRange);
    if (timeline_->model().selection()) {
        addSource(
            tr("Selected range"),
            exporting::ContactSheetSource::SelectedRange);
    }
    const auto detections = analysisManager_->detectionResults();
    if (!detections.scenes.empty()) {
        addSource(
            tr("Detected scenes"),
            exporting::ContactSheetSource::DetectedScenes);
    }
    bool accepted = false;
    const QString selectedSource = QInputDialog::getItem(
        this,
        tr("Create Contact Sheet"),
        tr("Frame source"),
        sourceLabels,
        0,
        false,
        &accepted);
    if (!accepted) {
        return;
    }
    const int sourceIndex = sourceLabels.indexOf(selectedSource);
    if (sourceIndex < 0
        || sourceIndex >= static_cast<int>(sources.size())) {
        return;
    }

    const QStringList presets{
        tr("8 frames"),
        tr("16 frames"),
        tr("20 frames"),
        tr("25 frames"),
        tr("Custom rows x columns"),
    };
    const QString preset = QInputDialog::getItem(
        this,
        tr("Create Contact Sheet"),
        tr("Layout preset"),
        presets,
        2,
        false,
        &accepted);
    if (!accepted) {
        return;
    }

    int rows = 0;
    int columns = 0;
    int frameCount = 0;
    const int presetIndex = presets.indexOf(preset);
    if (presetIndex >= 0 && presetIndex < 4) {
        constexpr int counts[]{8, 16, 20, 25};
        frameCount = counts[presetIndex];
        const QSize grid = exporting::ExportPlanner::presetGrid(frameCount);
        columns = grid.width();
        rows = grid.height();
    } else {
        rows = QInputDialog::getInt(
            this,
            tr("Custom Contact Sheet"),
            tr("Rows"),
            4,
            1,
            32,
            1,
            &accepted);
        if (!accepted) {
            return;
        }
        columns = QInputDialog::getInt(
            this,
            tr("Custom Contact Sheet"),
            tr("Columns"),
            5,
            1,
            32,
            1,
            &accepted);
        if (!accepted) {
            return;
        }
        frameCount = std::min(
            exporting::ExportPlanner::kMaximumContactSheetCells,
            rows * columns);
    }

    const bool includeTimestamp = QMessageBox::question(
        this,
        tr("Contact Sheet Labels"),
        tr("Include timestamps below each cell?"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes) == QMessageBox::Yes;
    const bool includeFrameIndex = QMessageBox::question(
        this,
        tr("Contact Sheet Labels"),
        tr("Include exact frame indices when known?"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes) == QMessageBox::Yes;

    QSettings settings;
    const QString directory = settings.value(
        QStringLiteral("export/lastDirectory"),
        QDir::homePath()).toString();
    const QString stem = exporting::ExportPlanner::sanitizedBaseName(
        pathToQString(mediaInfo_->path.stem()));
    const QString output = QFileDialog::getSaveFileName(
        this,
        tr("Save Contact Sheet"),
        QDir(directory).filePath(stem + QStringLiteral("_contact_sheet.png")),
        exporting::ExportPlanner::fileDialogFilter(false, false));
    if (output.isEmpty()) {
        return;
    }
    settings.setValue(
        QStringLiteral("export/lastDirectory"),
        QFileInfo(output).absolutePath());

    exporting::ExportRequest request;
    request.kind = exporting::ExportKind::ContactSheet;
    request.outputPath = qStringToPath(output);
    request.baseName = stem;
    request.format = imageFormatForPath(output);
    request.overwrite = true;
    request.rangeStart = media::MediaTime::zero();
    request.rangeEnd = mediaInfo_->duration;
    request.contactSheet.source = sources[static_cast<std::size_t>(sourceIndex)];
    request.contactSheet.rows = rows;
    request.contactSheet.columns = columns;
    request.contactSheet.frameCount = frameCount;
    request.contactSheet.includeTimestamp = includeTimestamp;
    request.contactSheet.includeFrameIndex = includeFrameIndex;
    switch (request.contactSheet.source) {
    case exporting::ContactSheetSource::EntireVideo:
        break;
    case exporting::ContactSheetSource::VisibleRange:
        request.rangeStart = timeline_->model().viewportStart();
        request.rangeEnd = timeline_->model().viewportEnd();
        break;
    case exporting::ContactSheetSource::SelectedRange: {
        const auto& selection = timeline_->model().selection();
        if (!selection) {
            return;
        }
        request.rangeStart = selection->start;
        request.rangeEnd = selection->end;
        break;
    }
    case exporting::ContactSheetSource::DetectedScenes:
        request.targetTimes.reserve(detections.scenes.size());
        for (const auto& scene : detections.scenes) {
            request.targetTimes.push_back(scene.start);
        }
        break;
    }
    startExport(std::move(request));
}

void MainWindow::startExport(exporting::ExportRequest request)
{
    if (exportManager_ == nullptr || !mediaInfo_) {
        return;
    }
    if (exportManager_->isBusy()) {
        QMessageBox::warning(
            this,
            tr("Export In Progress"),
            tr("Cancel or wait for the current export before starting another."));
        return;
    }
    if (request.baseName.isEmpty()) {
        request.baseName = pathToQString(mediaInfo_->path.stem());
    }
    controller_->pause();
    if (!exportManager_->startExport(std::move(request))) {
        QMessageBox::warning(
            this,
            tr("Could Not Start Export"),
            tr("The export worker is not ready for a new request."));
        return;
    }

    if (exportProgress_ != nullptr) {
        exportProgress_->close();
        exportProgress_->deleteLater();
    }
    exportProgress_ = new QProgressDialog(
        tr("Preparing full-resolution export..."),
        tr("Cancel"),
        0,
        0,
        this);
    exportProgress_->setObjectName(QStringLiteral("exportProgressDialog"));
    exportProgress_->setWindowTitle(tr("VidScope Export"));
    exportProgress_->setWindowModality(Qt::NonModal);
    exportProgress_->setAutoClose(false);
    exportProgress_->setAutoReset(false);
    exportProgress_->setMinimumDuration(0);
    connect(
        exportProgress_,
        &QProgressDialog::canceled,
        exportManager_,
        &exporting::ExportManager::cancel);
    exportProgress_->show();
    statusBar()->showMessage(tr("Export started"));
    updateExportActions();
}

void MainWindow::finishExport(const exporting::ExportSummary& summary)
{
    if (exportProgress_ != nullptr) {
        exportProgress_->close();
        exportProgress_->deleteLater();
        exportProgress_ = nullptr;
    }
    switch (summary.state) {
    case exporting::ExportState::Completed:
        statusBar()->showMessage(
            tr("Export complete: %1 file(s), %2 decoded frame(s)")
                .arg(summary.filesWritten)
                .arg(summary.framesDecoded),
            8000);
        break;
    case exporting::ExportState::Cancelled:
        statusBar()->showMessage(tr("Export cancelled"), 4000);
        break;
    case exporting::ExportState::Failed:
        statusBar()->showMessage(tr("Export failed"), 5000);
        QMessageBox::critical(
            this,
            tr("Export Failed"),
            summary.detail.isEmpty()
                ? tr("The export could not be completed.")
                : summary.detail);
        break;
    case exporting::ExportState::Idle:
    case exporting::ExportState::Running:
    case exporting::ExportState::Cancelling:
        break;
    }
    updateExportActions();
}

void MainWindow::updateExportActions()
{
    const bool hasMedia = mediaInfo_ != nullptr && !opening_;
    const bool busy = exportManager_ != nullptr && exportManager_->isBusy();
    const bool ready = hasMedia && !busy;
    const bool hasCurrent = ready && currentFrame_ != nullptr;
    const bool hasSelection =
        ready && timeline_ != nullptr && timeline_->model().selection().has_value();
    const bool hasScenes =
        ready && analysisManager_ != nullptr
        && !analysisManager_->detectionResults().scenes.empty();
    const bool hasAnalysis =
        ready && analysisManager_ != nullptr && analysisManager_->sampleCount() > 0;

    for (const char* name : {
             "actionExportCurrentFrame",
             "actionExportPreviousFrame",
             "actionExportNextFrame"}) {
        if (auto* action = actionByName(this, name)) {
            action->setEnabled(hasCurrent);
        }
    }
    for (const char* name : {
             "actionExportEveryNFrames",
             "actionExportKeyframes",
             "actionCreateContactSheet"}) {
        if (auto* action = actionByName(this, name)) {
            action->setEnabled(ready);
        }
    }
    if (auto* action = actionByName(this, "actionExportSelectedFrames")) {
        action->setEnabled(hasSelection);
    }
    if (auto* action = actionByName(this, "actionExportSceneFrames")) {
        action->setEnabled(hasScenes);
    }
    if (auto* action = actionByName(this, "actionExportHighMotionFrames")) {
        action->setEnabled(hasAnalysis);
    }
    if (auto* action = actionByName(this, "actionCancelExport")) {
        action->setEnabled(busy);
    }
}

void MainWindow::showPlaybackError(const QString& title, const QString& detail)
{
    opening_ = false;
    handleState(controller_->state());
    statusBar()->showMessage(title);
    QMessageBox::critical(this, title, detail);
}

void MainWindow::updateFrameStatus(const media::DecodedFrame& frame)
{
    const QString index = frame.id.presentationIndex >= 0
        ? QString::number(frame.id.presentationIndex)
        : QStringLiteral("?");
    const QString pts = frame.id.pts != AV_NOPTS_VALUE
        ? QString::number(frame.id.pts)
        : QStringLiteral("N/A");
    const QString dts = frame.dts != AV_NOPTS_VALUE
        ? QString::number(frame.dts)
        : QStringLiteral("N/A");
    const QString pictureType = QString::fromLatin1(media::pictureTypeName(frame.pictureType));
    const qint64 durationNs = std::max<qint64>(0, static_cast<qint64>(frame.duration.count()));
    const QString instantaneousRate = durationNs > 0
        ? QStringLiteral("%1 fps").arg(
              static_cast<double>(kNanosecondsPerSecond) / static_cast<double>(durationNs),
              0,
              'f',
              3)
        : QStringLiteral("VFR");
    QString masteringDisplay = tr("not present");
    if (frame.masteringDisplay) {
        if (frame.masteringDisplay->luminance) {
            masteringDisplay = tr("%1 to %2 nits")
                .arg(frame.masteringDisplay->luminance->minimumNits, 0, 'g', 8)
                .arg(frame.masteringDisplay->luminance->maximumNits, 0, 'g', 8);
        } else {
            masteringDisplay = tr("primaries only");
        }
    }
    const QString contentLight = frame.contentLight
        ? tr("MaxCLL %1 nits, MaxFALL %2 nits")
              .arg(frame.contentLight->maxContentLightLevel)
              .arg(frame.contentLight->maxFrameAverageLightLevel)
        : tr("not present");

    frameStatus_->setText(
        tr("Frame %1 | %2 | PTS %3 | DTS %4 | %5%6 | %7 | %8")
            .arg(index)
            .arg(formatTime(static_cast<qint64>(frame.presentationTime.count())))
            .arg(pts)
            .arg(dts)
            .arg(pictureType)
            .arg(frame.keyFrame ? QStringLiteral(" key") : QString{})
            .arg(pixelFormatName(frame.pixelFormat))
            .arg(instantaneousRate));

    frameStatus_->setToolTip(
        tr("Presentation index: %1\nPTS: %2\nDTS: %3\nDuration: %4 ns\n"
           "Dimensions: %5x%6\nPixel format: %7\nBit depth: %8\nKeyframe: %9\n"
           "Mastering display: %10\nContent light: %11")
            .arg(index)
            .arg(pts)
            .arg(dts)
            .arg(durationNs)
            .arg(frame.width)
            .arg(frame.height)
            .arg(pixelFormatName(frame.pixelFormat))
            .arg(frame.bitDepth)
            .arg(frame.keyFrame ? tr("yes") : tr("no"))
            .arg(masteringDisplay)
            .arg(contentLight));
}

void MainWindow::updateSelectionStatus()
{
    if (!timeline_ || !selectionStatus_ || !timeline_->model().selection()) {
        if (selectionStatus_) {
            selectionStatus_->setText(tr("Selection -"));
            selectionStatus_->setToolTip({});
        }
        updateExportActions();
        return;
    }

    const auto details = timeline_->model().selectionDetails();
    const QString firstFrame = details.firstFrame && details.firstFrame->presentationIndex >= 0
        ? QString::number(details.firstFrame->presentationIndex)
        : QStringLiteral("?");
    const QString lastFrame = details.lastFrame && details.lastFrame->presentationIndex >= 0
        ? QString::number(details.lastFrame->presentationIndex)
        : QStringLiteral("?");
    const QString count = details.frameCount
        ? tr("%1 frames").arg(*details.frameCount)
        : tr("%1 known").arg(details.knownFrameCount);

    selectionStatus_->setText(
        tr("In %1 | Out %2 | %3")
            .arg(formatTime(static_cast<qint64>(details.range.start.count())))
            .arg(formatTime(static_cast<qint64>(details.range.end.count())))
            .arg(count));
    selectionStatus_->setToolTip(
        tr("Frame %1 -> %2\n%3 -> %4\n%5\n"
           "Counts use only exact decoded/indexed frame identities; no FPS estimate is used.")
            .arg(firstFrame)
            .arg(lastFrame)
            .arg(formatTime(static_cast<qint64>(details.range.start.count())))
            .arg(formatTime(static_cast<qint64>(details.range.end.count())))
            .arg(count));
    updateExportActions();
}

void MainWindow::applyDetectionResults()
{
    if (analysisManager_ == nullptr || analysisResults_ == nullptr || timeline_ == nullptr) {
        return;
    }
    const auto results = analysisManager_->detectionResults();
    if (results.analyzedSamples == 0 && results.scenes.empty()
        && results.duplicates.empty() && results.freezes.empty()) {
        analysisResults_->clearResults();
    } else {
        analysisResults_->setResults(results);
    }
    timeline_->clearMarkers(timeline::TimelineMarkerKind::Scene);
    std::size_t index = 1;
    for (const auto& scene : results.scenes) {
        if (!timeline_->addMarker(
                static_cast<qint64>(scene.start.count()),
                timeline::TimelineMarkerKind::Scene,
                tr("Scene %1").arg(index++))) {
            break;
        }
    }
    updateExportActions();
}

void MainWindow::seekAdjacentScene(const bool forward)
{
    if (!timeline_) {
        return;
    }
    const auto target = timeline_->adjacentMarkerNanoseconds(
        timeline::TimelineMarkerKind::Scene,
        forward);
    if (!target) {
        statusBar()->showMessage(
            forward ? tr("No next scene marker is available")
                    : tr("No previous scene marker is available"),
            3000);
        return;
    }
    controller_->seekToNanoseconds(*target);
}

void MainWindow::showShortcutEditor()
{
    ShortcutEditorDialog dialog(findChildren<QAction*>(), this);
    dialog.exec();
}

} // namespace vidscope::widgets
