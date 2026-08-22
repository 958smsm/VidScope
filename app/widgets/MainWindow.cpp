#include "widgets/MainWindow.h"

#include "render/VideoViewport.h"
#include "timeline/TimelineWidget.h"
#include "thumbnails/ThumbnailManager.h"
#include "widgets/FilmstripController.h"
#include "widgets/FilmstripWidget.h"
#include "widgets/HoverPreviewController.h"
#include "widgets/ShortcutEditorDialog.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtGui/QAction>
#include <QtGui/QFontDatabase>
#include <QtGui/QIcon>
#include <QtGui/QIntValidator>
#include <QtGui/QKeySequence>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
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

    thumbnailManager_ = new thumbnails::ThumbnailManager({}, this);
    thumbnailManager_->setObjectName(QStringLiteral("thumbnailManager"));
    filmstripController_ = new FilmstripController(
        timeline_,
        thumbnailManager_,
        filmstrip_,
        {},
        this);
    hoverPreviewController_ = new HoverPreviewController(
        timeline_,
        thumbnailManager_,
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
            statusBar()->showMessage(
                presentationIndex >= 0
                    ? tr("Frame %1 selected for inspection (full inspector arrives in Phase 9).")
                          .arg(presentationIndex)
                    : tr("Frame selected for inspection (full inspector arrives in Phase 9)."),
                4000);
        });

    QSettings initialSettings;
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
        thumbnailManager_->clearMedia();
        viewport_->clearFrame();
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
    connect(timeline_, &timeline::TimelineWidget::scrubbingChanged, this, [this](bool active) {
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

    // Join all decode workers while their GUI receivers are still alive.
    delete hoverPreviewController_;
    hoverPreviewController_ = nullptr;
    delete filmstripController_;
    filmstripController_ = nullptr;
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
        QMessageBox::about(
            this,
            tr("About VidScope"),
            tr("<b>VidScope</b><br>Frame-accurate video inspection built with Qt and FFmpeg."));
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

    auto* metrics = new QLabel(tr("decode - | cache 0"), controls);
    metrics->setObjectName(QStringLiteral("metricsLabel"));
    metrics->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    inspection->addWidget(metrics);
    controlsLayout->addLayout(inspection);

    root->addWidget(controls);
    setCentralWidget(central);

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

    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(actionByName(this, "actionFullscreen"));

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
    thumbnailManager_->clearMedia();
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
    thumbnailManager_->setMedia(info);
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
    viewport_->setFrame(image);
    updateFrameStatus(*frame);
    updateSelectionStatus();
}

void MainWindow::handleState(playback::PlaybackState state)
{
    const bool hasMedia = !opening_
        && state != playback::PlaybackState::Closed
        && state != playback::PlaybackState::Error;
    const bool playing = state == playback::PlaybackState::Playing;

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
             "actionRefreshFilmstrip"}) {
        if (auto* action = actionByName(this, name)) {
            action->setEnabled(hasMedia);
        }
    }
    frameStepBox_->setEnabled(hasMedia);
    filmstripModeBox_->setEnabled(hasMedia);
    filmstripCountBox_->setEnabled(hasMedia);
    statusBar()->showMessage(stateDescription(state));
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
