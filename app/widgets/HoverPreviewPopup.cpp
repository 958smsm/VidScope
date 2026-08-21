#include "widgets/HoverPreviewPopup.h"

#include "media/MediaTypes.h"

#include <QtGui/QFontDatabase>
#include <QtGui/QGuiApplication>
#include <QtGui/QPixmap>
#include <QtGui/QScreen>
#include <QtCore/QRect>
#include <QtCore/QStringList>
#include <QtCore/Qt>
#include <QtWidgets/QFrame>
#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <optional>

namespace vidscope::widgets {
namespace {

constexpr qint64 kNanosecondsPerMillisecond = 1'000'000;
constexpr int kCursorGap = 14;

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

[[nodiscard]] QString formattedScore(const std::optional<float>& score)
{
    if (!score) {
        return HoverPreviewPopup::tr("not analyzed");
    }
    return QStringLiteral("%1%").arg(
        std::clamp(static_cast<double>(*score), 0.0, 1.0) * 100.0,
        0,
        'f',
        0);
}

[[nodiscard]] QString cacheDescription(thumbnails::ThumbnailCacheSource source)
{
    switch (source) {
    case thumbnails::ThumbnailCacheSource::Memory:
        return HoverPreviewPopup::tr("memory cache");
    case thumbnails::ThumbnailCacheSource::Disk:
        return HoverPreviewPopup::tr("disk cache");
    case thumbnails::ThumbnailCacheSource::Decoded:
        return HoverPreviewPopup::tr("decoded");
    case thumbnails::ThumbnailCacheSource::None:
        break;
    }
    return {};
}

} // namespace

HoverPreviewPopup::HoverPreviewPopup(QWidget* anchorWindow)
    : QWidget(anchorWindow, Qt::ToolTip | Qt::FramelessWindowHint)
{
    setObjectName(QStringLiteral("hoverPreviewPopup"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);

    auto* surface = new QFrame(this);
    surface->setObjectName(QStringLiteral("hoverPreviewSurface"));
    auto* surfaceLayout = new QVBoxLayout(surface);
    surfaceLayout->setContentsMargins(8, 8, 8, 8);
    surfaceLayout->setSpacing(5);

    imageLabel_ = new QLabel(tr("Loading preview..."), surface);
    imageLabel_->setObjectName(QStringLiteral("hoverPreviewImage"));
    imageLabel_->setAlignment(Qt::AlignCenter);
    imageLabel_->setFixedSize(320, 180);
    imageLabel_->setMinimumSize(320, 180);
    surfaceLayout->addWidget(imageLabel_);

    frameLabel_ = new QLabel(tr("Frame ?"), surface);
    frameLabel_->setObjectName(QStringLiteral("hoverPreviewFrame"));
    frameLabel_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    surfaceLayout->addWidget(frameLabel_);

    timeLabel_ = new QLabel(QStringLiteral("00:00:00.000"), surface);
    timeLabel_->setObjectName(QStringLiteral("hoverPreviewTime"));
    timeLabel_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    surfaceLayout->addWidget(timeLabel_);

    detailLabel_ = new QLabel(surface);
    detailLabel_->setObjectName(QStringLiteral("hoverPreviewDetail"));
    detailLabel_->setTextFormat(Qt::PlainText);
    detailLabel_->setWordWrap(true);
    surfaceLayout->addWidget(detailLabel_);

    analysisLabel_ = new QLabel(tr("Motion: not analyzed | Similarity: not analyzed"), surface);
    analysisLabel_->setObjectName(QStringLiteral("hoverPreviewAnalysis"));
    analysisLabel_->setTextFormat(Qt::PlainText);
    analysisLabel_->setWordWrap(true);
    surfaceLayout->addWidget(analysisLabel_);

    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->addWidget(surface);

    setStyleSheet(QStringLiteral(R"(
        QFrame#hoverPreviewSurface {
            background: #1a1f27;
            border: 1px solid #465161;
            border-radius: 6px;
        }
        QLabel#hoverPreviewImage {
            background: #090b0e;
            color: #8893a2;
            border: 1px solid #303844;
        }
        QLabel#hoverPreviewFrame { color: #e1e8f2; font-weight: 600; }
        QLabel#hoverPreviewTime { color: #8ec5f4; }
        QLabel#hoverPreviewDetail { color: #aeb8c6; }
        QLabel#hoverPreviewAnalysis { color: #7f8a99; }
    )"));
    setPreviewSize(QSize(320, 180));
    hide();
}

void HoverPreviewPopup::setPreviewSize(QSize size)
{
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0) {
        size = QSize(320, 180);
    }
    imageLabel_->setFixedSize(size);
    detailLabel_->setMaximumWidth(size.width());
    analysisLabel_->setMaximumWidth(size.width());
    adjustSize();
}

void HoverPreviewPopup::showPending(
    thumbnails::ThumbnailGeneration generation,
    qint64 timestampNanoseconds,
    qint64 presentationIndexHint,
    QPoint globalCursorPosition)
{
    generation_ = generation;
    displayedTimestampNanoseconds_ = std::max<qint64>(0, timestampNanoseconds);
    hasPreviewImage_ = false;
    imageLabel_->setPixmap(QPixmap{});
    imageLabel_->setText(tr("Loading preview..."));
    updateMetadata(
        displayedTimestampNanoseconds_,
        -1,
        presentationIndexHint,
        AV_PICTURE_TYPE_NONE,
        false,
        std::nullopt,
        std::nullopt,
        thumbnails::ThumbnailCacheSource::None);
    adjustSize();
    positionNear(globalCursorPosition);
    show();
    raise();
}

void HoverPreviewPopup::showPreview(
    const thumbnails::ThumbnailResult& result,
    QPoint globalCursorPosition)
{
    if (result.request.generation == 0 || result.request.generation != generation_
        || result.frame.image.isNull()) {
        return;
    }

    displayedTimestampNanoseconds_ = result.frame.presentationTime == media::kNoMediaTime
        ? static_cast<qint64>(result.request.timestamp.count())
        : static_cast<qint64>(result.frame.presentationTime.count());
    displayedTimestampNanoseconds_ = std::max<qint64>(0, displayedTimestampNanoseconds_);
    hasPreviewImage_ = true;
    imageLabel_->setText(QString{});
    const QPixmap pixmap = QPixmap::fromImage(result.frame.image);
    imageLabel_->setPixmap(pixmap.scaled(
        imageLabel_->size(),
        Qt::KeepAspectRatio,
        Qt::SmoothTransformation));
    updateMetadata(
        displayedTimestampNanoseconds_,
        result.frame.presentationIndex,
        result.request.presentationIndexHint,
        result.frame.pictureType,
        result.frame.keyFrame,
        result.frame.motionScore,
        result.frame.similarityScore,
        result.cacheSource);
    adjustSize();
    positionNear(globalCursorPosition);
    show();
    raise();
}

void HoverPreviewPopup::showFailure(
    thumbnails::ThumbnailGeneration generation,
    qint64 timestampNanoseconds,
    qint64 presentationIndexHint,
    const QString& detail,
    QPoint globalCursorPosition)
{
    if (generation == 0 || generation != generation_) {
        return;
    }

    displayedTimestampNanoseconds_ = std::max<qint64>(0, timestampNanoseconds);
    hasPreviewImage_ = false;
    imageLabel_->setPixmap(QPixmap{});
    imageLabel_->setText(tr("Preview unavailable"));
    updateMetadata(
        displayedTimestampNanoseconds_,
        -1,
        presentationIndexHint,
        AV_PICTURE_TYPE_NONE,
        false,
        std::nullopt,
        std::nullopt,
        thumbnails::ThumbnailCacheSource::None);
    QString boundedDetail = detail.simplified();
    constexpr qsizetype maximumDetailCharacters = 240;
    if (boundedDetail.size() > maximumDetailCharacters) {
        boundedDetail = boundedDetail.left(maximumDetailCharacters - 1) + QStringLiteral("\u2026");
    }
    detailLabel_->setText(boundedDetail);
    adjustSize();
    positionNear(globalCursorPosition);
    show();
    raise();
}

void HoverPreviewPopup::updateCursorPosition(QPoint globalCursorPosition)
{
    if (isVisible()) {
        positionNear(globalCursorPosition);
    }
}

void HoverPreviewPopup::dismiss()
{
    generation_ = 0;
    hasPreviewImage_ = false;
    hide();
}

thumbnails::ThumbnailGeneration HoverPreviewPopup::generation() const noexcept
{
    return generation_;
}

qint64 HoverPreviewPopup::displayedTimestampNanoseconds() const noexcept
{
    return displayedTimestampNanoseconds_;
}

bool HoverPreviewPopup::hasPreviewImage() const noexcept
{
    return hasPreviewImage_;
}

void HoverPreviewPopup::positionNear(QPoint globalCursorPosition)
{
    adjustSize();

    QScreen* screen = QGuiApplication::screenAt(globalCursorPosition);
    if (screen == nullptr && parentWidget() != nullptr) {
        screen = parentWidget()->screen();
    }
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        move(globalCursorPosition + QPoint(kCursorGap, kCursorGap));
        return;
    }

    const QRect screenBounds = screen->availableGeometry();
    QRect bounds = screenBounds;
    if (parentWidget() != nullptr) {
        QWidget* anchor = parentWidget()->window();
        const QRect anchorBounds(anchor->mapToGlobal(QPoint(0, 0)), anchor->size());
        const QRect intersection = anchorBounds.intersected(screenBounds);
        if (intersection.width() >= width() && intersection.height() >= height()) {
            bounds = intersection;
        }
    }

    int x = globalCursorPosition.x() + kCursorGap;
    int y = globalCursorPosition.y() - height() - kCursorGap;
    if (y < bounds.top()) {
        y = globalCursorPosition.y() + kCursorGap;
    }

    const int maximumX = std::max(bounds.left(), bounds.right() - width() + 1);
    const int maximumY = std::max(bounds.top(), bounds.bottom() - height() + 1);
    x = std::clamp(x, bounds.left(), maximumX);
    y = std::clamp(y, bounds.top(), maximumY);
    move(x, y);
}

void HoverPreviewPopup::updateMetadata(
    qint64 timestampNanoseconds,
    qint64 presentationIndex,
    qint64 presentationIndexHint,
    AVPictureType pictureType,
    bool keyFrame,
    std::optional<float> motionScore,
    std::optional<float> similarityScore,
    thumbnails::ThumbnailCacheSource cacheSource)
{
    if (presentationIndex >= 0) {
        frameLabel_->setText(tr("Frame %1").arg(presentationIndex));
    } else if (presentationIndexHint >= 0) {
        frameLabel_->setText(tr("Frame ~%1").arg(presentationIndexHint));
    } else {
        frameLabel_->setText(tr("Frame ?"));
    }
    timeLabel_->setText(formatTime(timestampNanoseconds));

    QStringList details;
    if (pictureType != AV_PICTURE_TYPE_NONE) {
        details.push_back(QString::fromLatin1(media::pictureTypeName(pictureType)));
    }
    if (keyFrame) {
        details.push_back(tr("keyframe"));
    }
    const QString cache = cacheDescription(cacheSource);
    if (!cache.isEmpty()) {
        details.push_back(cache);
    }
    detailLabel_->setText(details.join(QStringLiteral(" | ")));
    analysisLabel_->setText(
        tr("Motion: %1 | Similarity: %2")
            .arg(formattedScore(motionScore))
            .arg(formattedScore(similarityScore)));
}

} // namespace vidscope::widgets
