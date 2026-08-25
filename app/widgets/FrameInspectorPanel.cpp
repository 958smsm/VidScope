#include "widgets/FrameInspectorPanel.h"

#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>
#include <QtCore/QStringList>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace vidscope::widgets {
namespace {

constexpr qint64 kNanosecondsPerMillisecond = 1'000'000;

[[nodiscard]] QString formatTime(media::MediaTime time)
{
    const qint64 nanoseconds = std::max<qint64>(
        0,
        static_cast<qint64>(time.count()));
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

[[nodiscard]] QString optionalTimestamp(const std::int64_t value)
{
    return value == AV_NOPTS_VALUE ? QStringLiteral("N/A") : QString::number(value);
}

[[nodiscard]] QString namedValue(const char* value, const QString& fallback)
{
    return value != nullptr ? QString::fromLatin1(value) : fallback;
}

[[nodiscard]] QString normalizedScore(const std::optional<float> score)
{
    return score
        ? QStringLiteral("%1 (%2%)")
              .arg(static_cast<double>(*score), 0, 'f', 4)
              .arg(static_cast<double>(*score) * 100.0, 0, 'f', 1)
        : FrameInspectorPanel::tr("not analyzed");
}

[[nodiscard]] QString hdrDescription(const media::DecodedFrame& frame)
{
    QStringList values;
    if (frame.masteringDisplay) {
        if (frame.masteringDisplay->luminance) {
            values.push_back(
                FrameInspectorPanel::tr("mastering %1–%2 nits")
                    .arg(frame.masteringDisplay->luminance->minimumNits, 0, 'g', 8)
                    .arg(frame.masteringDisplay->luminance->maximumNits, 0, 'g', 8));
        }
        if (frame.masteringDisplay->primaries) {
            const auto& p = *frame.masteringDisplay->primaries;
            values.push_back(
                FrameInspectorPanel::tr(
                    "R(%1,%2) G(%3,%4) B(%5,%6) W(%7,%8)")
                    .arg(p.red.x, 0, 'f', 4)
                    .arg(p.red.y, 0, 'f', 4)
                    .arg(p.green.x, 0, 'f', 4)
                    .arg(p.green.y, 0, 'f', 4)
                    .arg(p.blue.x, 0, 'f', 4)
                    .arg(p.blue.y, 0, 'f', 4)
                    .arg(p.whitePoint.x, 0, 'f', 4)
                    .arg(p.whitePoint.y, 0, 'f', 4));
        }
    }
    if (frame.contentLight) {
        values.push_back(
            FrameInspectorPanel::tr("MaxCLL %1, MaxFALL %2 nits")
                .arg(frame.contentLight->maxContentLightLevel)
                .arg(frame.contentLight->maxFrameAverageLightLevel));
    }
    return values.isEmpty() ? FrameInspectorPanel::tr("not present")
                            : values.join(QStringLiteral(" | "));
}

[[nodiscard]] std::pair<double, double> lumaCoefficients(const AVColorSpace matrix) noexcept
{
    switch (matrix) {
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return {0.2627, 0.0593};
    case AVCOL_SPC_BT709:
        return {0.2126, 0.0722};
    default:
        return {0.2990, 0.1140};
    }
}

} // namespace

class PixelMagnifierWidget final : public QWidget {
public:
    explicit PixelMagnifierWidget(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("pixelMagnifier"));
        setMinimumSize(180, 180);
        setMaximumHeight(240);
    }

    void setImage(const QImage& image)
    {
        image_ = image;
        if (image_.isNull()) {
            sample_.reset();
        }
        update();
    }

    void setSample(const QPoint sample)
    {
        sample_ = sample;
        update();
    }

    void clearSample()
    {
        sample_.reset();
        update();
    }

    void setMagnification(const int magnification)
    {
        magnification_ = std::clamp(magnification, 2, 16);
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override
    {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.fillRect(rect(), QColor(11, 14, 18));
        painter.setPen(QPen(QColor(50, 58, 69), 1.0));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
        if (image_.isNull() || !sample_) {
            painter.setPen(QColor(121, 132, 147));
            painter.drawText(rect(), Qt::AlignCenter, tr("Hover the paused frame"));
            return;
        }

        const int sourceWidth = std::max(1, width() / magnification_);
        const int sourceHeight = std::max(1, height() / magnification_);
        const QRect source(
            sample_->x() - sourceWidth / 2,
            sample_->y() - sourceHeight / 2,
            sourceWidth,
            sourceHeight);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(rect(), image_, source);

        const double cellWidth =
            static_cast<double>(width()) / static_cast<double>(sourceWidth);
        const double cellHeight =
            static_cast<double>(height()) / static_cast<double>(sourceHeight);
        if (magnification_ >= 8) {
            painter.setPen(QPen(QColor(255, 255, 255, 65), 1.0));
            for (int x = 0; x <= sourceWidth; ++x) {
                const int destinationX = static_cast<int>(std::lround(x * cellWidth));
                painter.drawLine(destinationX, 0, destinationX, height());
            }
            for (int y = 0; y <= sourceHeight; ++y) {
                const int destinationY = static_cast<int>(std::lround(y * cellHeight));
                painter.drawLine(0, destinationY, width(), destinationY);
            }
        }
        const int selectedX = sample_->x() - source.left();
        const int selectedY = sample_->y() - source.top();
        painter.setPen(QPen(QColor(255, 208, 78), 2.0));
        painter.drawRect(QRectF(
            selectedX * cellWidth,
            selectedY * cellHeight,
            cellWidth,
            cellHeight).adjusted(1.0, 1.0, -1.0, -1.0));
    }

private:
    QImage image_;
    std::optional<QPoint> sample_;
    int magnification_ = 8;
};

FrameInspectorPanel::FrameInspectorPanel(QWidget* parent)
    : QWidget(parent)
    , comparisonManager_(new inspection::FrameComparisonManager(this))
{
    setObjectName(QStringLiteral("frameInspectorPanel"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(7);

    auto* navigation = new QHBoxLayout;
    previousFrame_ = new QPushButton(tr("Previous Frame"), this);
    previousFrame_->setObjectName(QStringLiteral("inspectorPreviousFrame"));
    nextFrame_ = new QPushButton(tr("Next Frame"), this);
    nextFrame_->setObjectName(QStringLiteral("inspectorNextFrame"));
    navigation->addWidget(previousFrame_);
    navigation->addWidget(nextFrame_);
    navigation->addStretch(1);
    navigation->addWidget(new QLabel(tr("Image zoom"), this));
    imageZoom_ = new QComboBox(this);
    imageZoom_->setObjectName(QStringLiteral("inspectorImageZoom"));
    imageZoom_->addItem(tr("Fit"), 0.0);
    imageZoom_->addItem(QStringLiteral("100%"), 1.0);
    imageZoom_->addItem(QStringLiteral("200%"), 2.0);
    imageZoom_->addItem(QStringLiteral("400%"), 4.0);
    navigation->addWidget(imageZoom_);
    root->addLayout(navigation);

    metadata_ = new QTreeWidget(this);
    metadata_->setObjectName(QStringLiteral("frameInspectorMetadata"));
    metadata_->setHeaderLabels({tr("Property"), tr("Value")});
    metadata_->setRootIsDecorated(false);
    metadata_->setAlternatingRowColors(true);
    metadata_->setSelectionMode(QAbstractItemView::NoSelection);
    metadata_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    metadata_->header()->setStretchLastSection(true);
    metadata_->setMinimumHeight(250);
    for (const auto& row : {
             std::pair{QStringLiteral("index"), tr("Frame index")},
             std::pair{QStringLiteral("timestamp"), tr("Timestamp")},
             std::pair{QStringLiteral("pts"), tr("PTS")},
             std::pair{QStringLiteral("dts"), tr("DTS")},
             std::pair{QStringLiteral("duration"), tr("Duration")},
             std::pair{QStringLiteral("type"), tr("Frame type")},
             std::pair{QStringLiteral("key"), tr("Keyframe")},
             std::pair{QStringLiteral("format"), tr("Pixel format")},
             std::pair{QStringLiteral("resolution"), tr("Resolution")},
             std::pair{QStringLiteral("depth"), tr("Bit depth")},
             std::pair{QStringLiteral("range"), tr("Color range")},
             std::pair{QStringLiteral("matrix"), tr("Matrix")},
             std::pair{QStringLiteral("primaries"), tr("Primaries")},
             std::pair{QStringLiteral("transfer"), tr("Transfer")},
             std::pair{QStringLiteral("hdr"), tr("HDR metadata")},
             std::pair{QStringLiteral("motion"), tr("Motion")},
             std::pair{QStringLiteral("similarity"), tr("Similarity")},
             std::pair{QStringLiteral("scene"), tr("Scene score")}}) {
        addMetadataRow(row.first, row.second);
    }
    root->addWidget(metadata_, 2);

    auto* pixelGroup = new QGroupBox(tr("Pixel Inspector"), this);
    pixelGroup->setObjectName(QStringLiteral("pixelInspectorGroup"));
    auto* pixelLayout = new QVBoxLayout(pixelGroup);
    auto* pixelControls = new QHBoxLayout;
    pixelEnabled_ = new QCheckBox(tr("Inspect paused frame"), pixelGroup);
    pixelEnabled_->setObjectName(QStringLiteral("pixelInspectorEnabled"));
    pixelControls->addWidget(pixelEnabled_);
    pixelControls->addStretch(1);
    pixelControls->addWidget(new QLabel(tr("Magnification"), pixelGroup));
    pixelMagnification_ = new QComboBox(pixelGroup);
    pixelMagnification_->setObjectName(QStringLiteral("pixelMagnification"));
    for (const int magnification : {2, 4, 8, 16}) {
        pixelMagnification_->addItem(
            QStringLiteral("%1×").arg(magnification),
            magnification);
    }
    pixelMagnification_->setCurrentIndex(2);
    pixelControls->addWidget(pixelMagnification_);
    pixelLayout->addLayout(pixelControls);
    magnifier_ = new PixelMagnifierWidget(pixelGroup);
    pixelLayout->addWidget(magnifier_);
    pixelReadout_ = new QLabel(tr("X: —  Y: —\nRGB: —\nYUV (display): —"), pixelGroup);
    pixelReadout_->setObjectName(QStringLiteral("pixelReadout"));
    pixelReadout_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pixelLayout->addWidget(pixelReadout_);
    root->addWidget(pixelGroup);

    auto* comparisonGroup = new QGroupBox(tr("A/B Comparison"), this);
    comparisonGroup->setObjectName(QStringLiteral("comparisonGroup"));
    auto* comparisonLayout = new QVBoxLayout(comparisonGroup);
    auto* captureButtons = new QHBoxLayout;
    setFrameA_ = new QPushButton(tr("Set Frame A"), comparisonGroup);
    setFrameA_->setObjectName(QStringLiteral("setFrameA"));
    setFrameB_ = new QPushButton(tr("Set Frame B"), comparisonGroup);
    setFrameB_->setObjectName(QStringLiteral("setFrameB"));
    clearComparison_ = new QPushButton(tr("Clear"), comparisonGroup);
    clearComparison_->setObjectName(QStringLiteral("clearFrameComparison"));
    captureButtons->addWidget(setFrameA_);
    captureButtons->addWidget(setFrameB_);
    captureButtons->addWidget(clearComparison_);
    comparisonLayout->addLayout(captureButtons);
    frameALabel_ = new QLabel(tr("A: not set"), comparisonGroup);
    frameALabel_->setObjectName(QStringLiteral("frameALabel"));
    frameBLabel_ = new QLabel(tr("B: not set"), comparisonGroup);
    frameBLabel_->setObjectName(QStringLiteral("frameBLabel"));
    comparisonLayout->addWidget(frameALabel_);
    comparisonLayout->addWidget(frameBLabel_);
    auto* modeForm = new QFormLayout;
    comparisonMode_ = new QComboBox(comparisonGroup);
    comparisonMode_->setObjectName(QStringLiteral("comparisonMode"));
    comparisonMode_->addItem(
        tr("Side by side"),
        static_cast<int>(inspection::ComparisonMode::SideBySide));
    comparisonMode_->addItem(
        tr("Overlay"),
        static_cast<int>(inspection::ComparisonMode::Overlay));
    comparisonMode_->addItem(
        tr("Wipe"),
        static_cast<int>(inspection::ComparisonMode::Wipe));
    comparisonMode_->addItem(
        tr("Blink"),
        static_cast<int>(inspection::ComparisonMode::Blink));
    comparisonMode_->addItem(
        tr("Absolute difference"),
        static_cast<int>(inspection::ComparisonMode::AbsoluteDifference));
    comparisonMode_->addItem(
        tr("Amplified difference (4×)"),
        static_cast<int>(inspection::ComparisonMode::AmplifiedDifference));
    comparisonMode_->addItem(
        tr("SSIM map"),
        static_cast<int>(inspection::ComparisonMode::SsimMap));
    modeForm->addRow(tr("Mode"), comparisonMode_);
    comparisonLayout->addLayout(modeForm);
    comparisonMetricsLabel_ = new QLabel(tr("SSIM: — | PSNR: —"), comparisonGroup);
    comparisonMetricsLabel_->setObjectName(QStringLiteral("comparisonMetrics"));
    comparisonMetricsLabel_->setWordWrap(true);
    comparisonMetricsLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    comparisonLayout->addWidget(comparisonMetricsLabel_);
    root->addWidget(comparisonGroup);

    connect(previousFrame_, &QPushButton::clicked, this, &FrameInspectorPanel::previousFrameRequested);
    connect(nextFrame_, &QPushButton::clicked, this, &FrameInspectorPanel::nextFrameRequested);
    connect(imageZoom_, &QComboBox::currentIndexChanged, this, [this](const int index) {
        emit imageZoomChanged(imageZoom_->itemData(index).toDouble());
    });
    connect(pixelEnabled_, &QCheckBox::toggled, this, [this](const bool enabled) {
        if (!enabled) {
            clearPixel();
        }
        emit pixelInspectionChanged(enabled);
    });
    connect(pixelMagnification_, &QComboBox::currentIndexChanged, this, [this](const int index) {
        magnifier_->setMagnification(pixelMagnification_->itemData(index).toInt());
    });
    connect(setFrameA_, &QPushButton::clicked, this, &FrameInspectorPanel::setCurrentAsFrameA);
    connect(setFrameB_, &QPushButton::clicked, this, &FrameInspectorPanel::setCurrentAsFrameB);
    connect(clearComparison_, &QPushButton::clicked, this, &FrameInspectorPanel::clearComparison);
    connect(comparisonMode_, &QComboBox::currentIndexChanged, this, [this](int) {
        requestComparison();
    });
    connect(
        comparisonManager_,
        &inspection::FrameComparisonManager::comparisonReady,
        this,
        [this](const inspection::ComparisonResult& result) {
            comparisonMetrics_ = result.metrics;
            if (result.metrics.comparable) {
                const QString psnr = std::isinf(result.metrics.psnrDb)
                    ? tr("infinite")
                    : QStringLiteral("%1 dB").arg(result.metrics.psnrDb, 0, 'f', 2);
                comparisonMetricsLabel_->setText(
                    tr("SSIM: %1 | PSNR: %2 | MSE: %3")
                        .arg(result.metrics.ssim, 0, 'f', 4)
                        .arg(psnr)
                        .arg(result.metrics.meanSquaredError, 0, 'f', 3));
            } else {
                comparisonMetricsLabel_->setText(result.metrics.detail);
            }
            emit comparisonDisplayChanged(
                imageA_,
                imageB_,
                result.mode,
                result.visualization,
                result.metrics.detail);
        });

    clear();
}

FrameInspectorPanel::~FrameInspectorPanel() = default;

void FrameInspectorPanel::addMetadataRow(const QString& key, const QString& label)
{
    auto* item = new QTreeWidgetItem(metadata_);
    item->setText(0, label);
    item->setText(1, QStringLiteral("—"));
    metadataRows_.insert(key, item);
}

void FrameInspectorPanel::setMetadata(const QString& key, const QString& value)
{
    if (auto* item = metadataRows_.value(key, nullptr)) {
        item->setText(1, value);
        item->setToolTip(1, value);
    }
}

void FrameInspectorPanel::setFrame(
    media::DecodedFramePtr frame,
    QImage image,
    std::optional<analysis::AnalysisSample> analysis)
{
    currentFrame_ = std::move(frame);
    currentImage_ = std::move(image);
    analysis_ = std::move(analysis);
    magnifier_->setImage(currentImage_);
    clearPixel();
    updateMetadata();
    updatePixelAvailability();
    setFrameA_->setEnabled(hasCurrentFrame());
    setFrameB_->setEnabled(hasCurrentFrame());
}

void FrameInspectorPanel::setAnalysis(std::optional<analysis::AnalysisSample> analysis)
{
    analysis_ = std::move(analysis);
    setMetadata(
        QStringLiteral("motion"),
        normalizedScore(analysis_ ? analysis_->motion : std::nullopt));
    setMetadata(
        QStringLiteral("similarity"),
        normalizedScore(analysis_ ? analysis_->similarity : std::nullopt));
    setMetadata(
        QStringLiteral("scene"),
        normalizedScore(analysis_ ? analysis_->sceneScore : std::nullopt));
}

void FrameInspectorPanel::setPaused(const bool paused)
{
    paused_ = paused;
    updatePixelAvailability();
}

void FrameInspectorPanel::clear()
{
    currentFrame_.reset();
    currentImage_ = {};
    analysis_.reset();
    magnifier_->setImage({});
    for (auto* item : metadataRows_) {
        item->setText(1, QStringLiteral("—"));
        item->setToolTip(1, {});
    }
    setFrameA_->setEnabled(false);
    setFrameB_->setEnabled(false);
    clearPixel();
    clearComparison();
    updatePixelAvailability();
}

void FrameInspectorPanel::updatePixel(int x, int y, const QColor& rgb)
{
    if (!pixelEnabled_->isChecked() || !currentFrame_ || currentImage_.isNull()
        || x < 0 || y < 0 || x >= currentImage_.width() || y >= currentImage_.height()) {
        return;
    }
    const auto [redCoefficient, blueCoefficient] =
        lumaCoefficients(currentFrame_->colorSpace);
    const double greenCoefficient = 1.0 - redCoefficient - blueCoefficient;
    const double red = static_cast<double>(rgb.red());
    const double green = static_cast<double>(rgb.green());
    const double blue = static_cast<double>(rgb.blue());
    const double lumaValue =
        redCoefficient * red + greenCoefficient * green + blueCoefficient * blue;
    const int yValue = std::clamp(static_cast<int>(std::lround(lumaValue)), 0, 255);
    const int uValue = std::clamp(
        static_cast<int>(std::lround(
            128.0 + 0.5 * (blue - lumaValue) / (1.0 - blueCoefficient))),
        0,
        255);
    const int vValue = std::clamp(
        static_cast<int>(std::lround(
            128.0 + 0.5 * (red - lumaValue) / (1.0 - redCoefficient))),
        0,
        255);
    pixelReadout_->setText(
        tr("X: %1  Y: %2\nRGB: %3, %4, %5\nYUV (display): %6, %7, %8")
            .arg(x)
            .arg(y)
            .arg(rgb.red())
            .arg(rgb.green())
            .arg(rgb.blue())
            .arg(yValue)
            .arg(uValue)
            .arg(vValue));
    magnifier_->setSample(QPoint(x, y));
}

void FrameInspectorPanel::clearPixel()
{
    pixelReadout_->setText(tr("X: —  Y: —\nRGB: —\nYUV (display): —"));
    magnifier_->clearSample();
}

bool FrameInspectorPanel::hasCurrentFrame() const noexcept
{
    return currentFrame_ != nullptr && !currentImage_.isNull();
}

bool FrameInspectorPanel::hasFrameA() const noexcept
{
    return frameA_ != nullptr && !imageA_.isNull();
}

bool FrameInspectorPanel::hasFrameB() const noexcept
{
    return frameB_ != nullptr && !imageB_.isNull();
}

inspection::ComparisonMode FrameInspectorPanel::comparisonMode() const noexcept
{
    return static_cast<inspection::ComparisonMode>(
        comparisonMode_->currentData().toInt());
}

const inspection::ComparisonMetrics& FrameInspectorPanel::comparisonMetrics() const noexcept
{
    return comparisonMetrics_;
}

void FrameInspectorPanel::setCurrentAsFrameA()
{
    if (!hasCurrentFrame()) {
        return;
    }
    frameA_ = currentFrame_;
    imageA_ = currentImage_;
    updateCaptureLabels();
    requestComparison();
}

void FrameInspectorPanel::setCurrentAsFrameB()
{
    if (!hasCurrentFrame()) {
        return;
    }
    frameB_ = currentFrame_;
    imageB_ = currentImage_;
    updateCaptureLabels();
    requestComparison();
}

void FrameInspectorPanel::clearComparison()
{
    comparisonManager_->cancel();
    frameA_.reset();
    frameB_.reset();
    imageA_ = {};
    imageB_ = {};
    comparisonMetrics_ = {};
    comparisonMetricsLabel_->setText(tr("SSIM: — | PSNR: —"));
    updateCaptureLabels();
    clearComparison_->setEnabled(false);
    emit comparisonCleared();
    updatePixelAvailability();
}

void FrameInspectorPanel::updateMetadata()
{
    if (!currentFrame_) {
        return;
    }
    const auto& frame = *currentFrame_;
    const char* formatName = av_get_pix_fmt_name(frame.pixelFormat);
    setMetadata(
        QStringLiteral("index"),
        frame.id.presentationIndex >= 0
            ? QString::number(frame.id.presentationIndex)
            : QStringLiteral("?"));
    setMetadata(QStringLiteral("timestamp"), formatTime(frame.presentationTime));
    setMetadata(QStringLiteral("pts"), optionalTimestamp(frame.id.pts));
    setMetadata(QStringLiteral("dts"), optionalTimestamp(frame.dts));
    setMetadata(
        QStringLiteral("duration"),
        tr("%1 ns (%2)")
            .arg(std::max<qint64>(0, static_cast<qint64>(frame.duration.count())))
            .arg(formatTime(frame.duration)));
    setMetadata(
        QStringLiteral("type"),
        QString::fromLatin1(media::pictureTypeName(frame.pictureType)));
    setMetadata(QStringLiteral("key"), frame.keyFrame ? tr("yes") : tr("no"));
    setMetadata(
        QStringLiteral("format"),
        formatName != nullptr ? QString::fromLatin1(formatName) : tr("unknown"));
    setMetadata(
        QStringLiteral("resolution"),
        QStringLiteral("%1 × %2").arg(frame.width).arg(frame.height));
    setMetadata(QStringLiteral("depth"), tr("%1-bit").arg(frame.bitDepth));
    setMetadata(
        QStringLiteral("range"),
        namedValue(av_color_range_name(frame.colorRange), tr("unspecified")));
    setMetadata(
        QStringLiteral("matrix"),
        namedValue(av_color_space_name(frame.colorSpace), tr("unspecified")));
    setMetadata(
        QStringLiteral("primaries"),
        namedValue(av_color_primaries_name(frame.colorPrimaries), tr("unspecified")));
    setMetadata(
        QStringLiteral("transfer"),
        namedValue(av_color_transfer_name(frame.colorTransfer), tr("unspecified")));
    setMetadata(QStringLiteral("hdr"), hdrDescription(frame));
    setAnalysis(analysis_);
}

void FrameInspectorPanel::updateCaptureLabels()
{
    frameALabel_->setText(
        hasFrameA() ? tr("A: %1").arg(frameIdentity(frameA_)) : tr("A: not set"));
    frameBLabel_->setText(
        hasFrameB() ? tr("B: %1").arg(frameIdentity(frameB_)) : tr("B: not set"));
    clearComparison_->setEnabled(hasFrameA() || hasFrameB());
}

void FrameInspectorPanel::updatePixelAvailability()
{
    const bool available = paused_ && hasCurrentFrame() && !(hasFrameA() && hasFrameB());
    pixelEnabled_->setEnabled(available);
    if (!available && pixelEnabled_->isChecked()) {
        pixelEnabled_->setChecked(false);
    }
}

void FrameInspectorPanel::requestComparison()
{
    updateCaptureLabels();
    if (!hasFrameA() || !hasFrameB()) {
        comparisonManager_->cancel();
        comparisonMetrics_ = {};
        comparisonMetricsLabel_->setText(tr("Set both A and B to compare."));
        emit comparisonCleared();
        updatePixelAvailability();
        return;
    }
    if (pixelEnabled_->isChecked()) {
        pixelEnabled_->setChecked(false);
    }
    const auto mode = comparisonMode();
    comparisonMetricsLabel_->setText(tr("Computing SSIM and PSNR…"));
    emit comparisonDisplayChanged(
        imageA_,
        imageB_,
        mode,
        {},
        tr("Computing comparison…"));
    (void)comparisonManager_->request(imageA_, imageB_, mode, 4);
    updatePixelAvailability();
}

QString FrameInspectorPanel::frameIdentity(const media::DecodedFramePtr& frame) const
{
    if (!frame) {
        return tr("not set");
    }
    const QString index = frame->id.presentationIndex >= 0
        ? QString::number(frame->id.presentationIndex)
        : QStringLiteral("?");
    return tr("Frame %1 at %2").arg(index).arg(formatTime(frame->presentationTime));
}

} // namespace vidscope::widgets
