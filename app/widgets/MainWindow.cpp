#include "widgets/MainWindow.h"

#include "render/VideoViewport.h"
#include "widgets/SeekBar.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtGui/QAction>
#include <QtGui/QFontDatabase>
#include <QtGui/QIcon>
#include <QtGui/QIntValidator>
#include <QtGui/QKeySequence>
#include <QtGui/QShortcut>
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
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
#include <filesystem>
#include <limits>

namespace vidscope::widgets {
namespace {

constexpr qint64 kNanosecondsPerMillisecond = 1'000'000;
constexpr qint64 kNanosecondsPerSecond = 1'000'000'000;
constexpr int kMaximumFrameStepCount = 1'000;

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
    setMinimumSize(900, 620);
    resize(1200, 800);

    createActions();
    createLayout();
    createMenus();
    createShortcuts();

    connect(controller_, &playback::PlaybackController::mediaOpened,
            this, &MainWindow::handleMediaOpened);
    connect(controller_, &playback::PlaybackController::frameReady,
            this, &MainWindow::handleFrame);
    connect(controller_, &playback::PlaybackController::stateChanged,
            this, &MainWindow::handleState);
    connect(controller_, &playback::PlaybackController::errorOccurred,
            this, &MainWindow::showPlaybackError);
    connect(controller_, &playback::PlaybackController::mediaClosed, this, [this] {
        viewport_->clearFrame();
        seekBar_->setDuration(0);
        mediaStatus_->setText(tr("No media loaded"));
        frameStatus_->setText(tr("Frame -"));
        setWindowTitle(QStringLiteral("VidScope"));
        if (auto* position = findChild<QLabel*>(QStringLiteral("positionLabel"))) {
            position->setProperty("positionNs", QVariant::fromValue<qint64>(0));
            position->setProperty("durationNs", QVariant::fromValue<qint64>(0));
            position->setText(QStringLiteral("00:00:00.000 / 00:00:00.000"));
        }
    });
    connect(controller_, &playback::PlaybackController::durationChanged, this, [this](qint64 duration) {
        seekBar_->setDuration(duration);
        if (auto* position = findChild<QLabel*>(QStringLiteral("positionLabel"))) {
            position->setProperty("durationNs", QVariant::fromValue(duration));
            const auto current = position->property("positionNs").toLongLong();
            position->setText(formatTime(current) + QStringLiteral(" / ") + formatTime(duration));
        }
    });
    connect(controller_, &playback::PlaybackController::positionChanged, this, [this](qint64 positionNs) {
        seekBar_->setPosition(positionNs);
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
                        ? tr(" | seek %1 ms").arg(static_cast<double>(seekMicroseconds) / 1'000.0, 0, 'f', 1)
                        : QString{};
                    metrics->setText(
                        tr("decode %1 fps%2 | cache %3")
                            .arg(decodeFps, 0, 'f', 1)
                            .arg(seekText)
                            .arg(cachedFrames));
                }
            });
    connect(seekBar_, &SeekBar::seekRequested,
            controller_, &playback::PlaybackController::seekToNanoseconds);
    connect(seekBar_, &SeekBar::scrubbingChanged, this, [this](bool active) {
        statusBar()->showMessage(active ? tr("Scrubbing") : stateDescription(controller_->state()));
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

    // Join the decode worker while all GUI receivers are still alive.
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

    auto* fullscreen = createAction("actionFullscreen", tr("&Full Screen"));
    fullscreen->setCheckable(true);
    connect(fullscreen, &QAction::toggled, this, [this](bool enabled) {
        enabled ? showFullScreen() : showNormal();
    });

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

    seekBar_ = new SeekBar(controls);
    controlsLayout->addWidget(seekBar_);

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
    inspection->addWidget(frameStatus_, 1);

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
        QLabel#metricsLabel { color: #7f8a99; }
        QLabel#positionLabel { color: #dce7f5; }
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
    playbackMenu->addAction(actionByName(this, "actionStop"));
    playbackMenu->addSeparator();
    playbackMenu->addAction(actionByName(this, "actionPreviousFrame"));
    playbackMenu->addAction(actionByName(this, "actionNextFrame"));
    playbackMenu->addAction(actionByName(this, "actionJumpBack"));
    playbackMenu->addAction(actionByName(this, "actionJumpForward"));
    playbackMenu->addSeparator();
    playbackMenu->addAction(actionByName(this, "actionPreviousKeyframe"));
    playbackMenu->addAction(actionByName(this, "actionNextKeyframe"));

    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(actionByName(this, "actionFullscreen"));

    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(actionByName(this, "actionAbout"));
}

void MainWindow::createShortcuts()
{
    actionByName(this, "actionOpen")->setShortcut(QKeySequence::Open);
    actionByName(this, "actionExit")->setShortcut(QKeySequence::Quit);
    actionByName(this, "actionPlayPause")
        ->setShortcut(QKeySequence(Qt::Key_Space));
    actionByName(this, "actionStop")->setShortcut(QKeySequence(Qt::Key_S));
    actionByName(this, "actionPreviousFrame")
        ->setShortcuts({
            QKeySequence(Qt::Key_Left),
            QKeySequence(Qt::Key_Comma),
            QKeySequence(Qt::Key_J)});
    actionByName(this, "actionNextFrame")
        ->setShortcuts({
            QKeySequence(Qt::Key_Right),
            QKeySequence(Qt::Key_Period),
            QKeySequence(Qt::Key_L)});
    actionByName(this, "actionPreviousKeyframe")
        ->setShortcut(QKeySequence(QKeyCombination(Qt::ControlModifier, Qt::Key_Left)));
    actionByName(this, "actionNextKeyframe")
        ->setShortcut(QKeySequence(QKeyCombination(Qt::ControlModifier, Qt::Key_Right)));
    actionByName(this, "actionJumpBack")
        ->setShortcut(QKeySequence(QKeyCombination(Qt::ShiftModifier, Qt::Key_Left)));
    actionByName(this, "actionJumpForward")
        ->setShortcut(QKeySequence(QKeyCombination(Qt::ShiftModifier, Qt::Key_Right)));
    actionByName(this, "actionFullscreen")->setShortcut(QKeySequence(Qt::Key_F11));

    auto* pause = new QShortcut(QKeySequence(Qt::Key_K), this);
    pause->setContext(Qt::WindowShortcut);
    connect(pause, &QShortcut::activated, controller_, &playback::PlaybackController::pause);
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
    statusBar()->showMessage(tr("Opening %1...").arg(QFileInfo(path).fileName()));
    controller_->openFile(path);
}

void MainWindow::handleMediaOpened(media::MediaInfoPtr info)
{
    if (!info) {
        return;
    }

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
    if (!frame || image.isNull()) {
        return;
    }
    viewport_->setFrame(image);
    updateFrameStatus(*frame);
}

void MainWindow::handleState(playback::PlaybackState state)
{
    const bool hasMedia = state != playback::PlaybackState::Closed
        && state != playback::PlaybackState::Error;
    const bool playing = state == playback::PlaybackState::Playing;

    if (auto* playPause = actionByName(this, "actionPlayPause")) {
        playPause->setEnabled(hasMedia);
        playPause->setText(playing ? tr("&Pause") : tr("&Play"));
        playPause->setToolTip(playing ? tr("Pause (Space or K)") : tr("Play (Space or K)"));
        playPause->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause
                                                        : QStyle::SP_MediaPlay));
    }
    for (const char* name : {
             "actionStop",
             "actionPreviousFrame",
             "actionNextFrame",
             "actionJumpBack",
             "actionJumpForward",
             "actionPreviousKeyframe",
             "actionNextKeyframe"}) {
        if (auto* action = actionByName(this, name)) {
            action->setEnabled(hasMedia);
        }
    }
    frameStepBox_->setEnabled(hasMedia);
    statusBar()->showMessage(stateDescription(state));
}

void MainWindow::showPlaybackError(const QString& title, const QString& detail)
{
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

} // namespace vidscope::widgets

