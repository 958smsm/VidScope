#include "widgets/FilmstripWidget.h"

#include "media/MediaTypes.h"

#include <QtCore/QEvent>
#include <QtCore/QSizeF>
#include <QtGui/QColor>
#include <QtGui/QFontMetrics>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtGui/QPaintEvent>
#include <QtGui/QPen>
#include <QtWidgets/QSizePolicy>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace vidscope::widgets {
namespace {

constexpr qreal kOuterMargin = 7.0;
constexpr qreal kItemGap = 5.0;
constexpr qreal kItemRadius = 5.0;
constexpr int kMinimumItemWidth = 112;
constexpr int kPreferredItemWidth = 156;
constexpr int kMaximumItemWidth = 228;
constexpr int kPreferredHeight = 132;
constexpr qint64 kNanosecondsPerMillisecond = 1'000'000;

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

[[nodiscard]] QString pictureTypeText(const AVPictureType type)
{
    const char* name = media::pictureTypeName(type);
    return name != nullptr ? QString::fromLatin1(name) : QStringLiteral("?");
}

[[nodiscard]] std::uint64_t absoluteDistance(qint64 left, qint64 right) noexcept
{
    const auto unsignedLeft = static_cast<std::uint64_t>(std::max<qint64>(0, left));
    const auto unsignedRight = static_cast<std::uint64_t>(std::max<qint64>(0, right));
    return unsignedLeft >= unsignedRight
        ? unsignedLeft - unsignedRight
        : unsignedRight - unsignedLeft;
}

[[nodiscard]] QRectF fittedImageRect(const QRectF bounds, const QSize imageSize) noexcept
{
    if (bounds.isEmpty() || !imageSize.isValid()) {
        return {};
    }

    const qreal widthScale = bounds.width() / static_cast<qreal>(imageSize.width());
    const qreal heightScale = bounds.height() / static_cast<qreal>(imageSize.height());
    const qreal scale = std::min(widthScale, heightScale);
    const QSizeF fitted(
        static_cast<qreal>(imageSize.width()) * scale,
        static_cast<qreal>(imageSize.height()) * scale);
    return QRectF(
        bounds.center().x() - fitted.width() / 2.0,
        bounds.center().y() - fitted.height() / 2.0,
        fitted.width(),
        fitted.height());
}

} // namespace

FilmstripWidget::FilmstripWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("filmstripWidget"));
    setAccessibleName(tr("Preview filmstrip"));
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
    setMinimumHeight(118);
    setMaximumHeight(158);
    clear(tr("Open a video to populate the preview filmstrip."));
}

void FilmstripWidget::setPlan(filmstrip::FilmstripPlan plan)
{
    plan_ = std::move(plan);
    items_.clear();
    selectedIndex_.reset();
    pressedIndex_.reset();
    hoveredIndex_.reset();

    switch (plan_.status) {
    case filmstrip::FilmstripPlanStatus::NoMedia:
        emptyMessage_ = tr("Open a video to populate the preview filmstrip.");
        break;
    case filmstrip::FilmstripPlanStatus::SelectionRequired:
        emptyMessage_ = tr("Create an In/Out timeline selection for Selected Range mode.");
        break;
    case filmstrip::FilmstripPlanStatus::Ready:
        emptyMessage_ = plan_.targets.empty()
            ? tr("No preview targets are available for this range.")
            : QString{};
        break;
    }

    items_.reserve(plan_.targets.size());
    for (const auto& target : plan_.targets) {
        items_.push_back(FilmstripItem{target, FilmstripItemState::Loading, std::nullopt, {}});
    }
    updateGeometry();
    update();
}

void FilmstripWidget::clear(QString message)
{
    plan_ = {};
    items_.clear();
    selectedIndex_.reset();
    pressedIndex_.reset();
    hoveredIndex_.reset();
    emptyMessage_ = message.isEmpty()
        ? tr("Open a video to populate the preview filmstrip.")
        : std::move(message);
    updateGeometry();
    update();
}

bool FilmstripWidget::setThumbnail(
    const std::size_t index,
    thumbnails::ThumbnailFrame frame)
{
    if (index >= items_.size() || frame.image.isNull()) {
        return false;
    }
    auto& item = items_[index];
    item.frame = std::move(frame);
    item.state = FilmstripItemState::Ready;
    item.failureDetail.clear();
    update(itemRect(index).toAlignedRect());
    return true;
}

bool FilmstripWidget::setAnalysis(
    const std::size_t index,
    std::optional<float> motion,
    std::optional<float> similarity)
{
    if (index >= items_.size() || !items_[index].frame) {
        return false;
    }
    auto& frame = *items_[index].frame;
    frame.motionScore = std::move(motion);
    frame.similarityScore = std::move(similarity);
    update(itemRect(index).toAlignedRect());
    if (hoveredIndex_ && *hoveredIndex_ == index) {
        updateItemToolTip(index);
    }
    return true;
}

bool FilmstripWidget::setFailure(const std::size_t index, QString detail)
{
    if (index >= items_.size()) {
        return false;
    }
    auto& item = items_[index];
    item.frame.reset();
    item.state = FilmstripItemState::Failed;
    item.failureDetail = detail.isEmpty() ? tr("Preview unavailable") : std::move(detail);
    update(itemRect(index).toAlignedRect());
    return true;
}

void FilmstripWidget::setPlayhead(qint64 nanoseconds)
{
    nanoseconds = std::max<qint64>(0, nanoseconds);
    if (playheadNanoseconds_ == nanoseconds) {
        return;
    }
    playheadNanoseconds_ = nanoseconds;
    update();
}

const filmstrip::FilmstripPlan& FilmstripWidget::plan() const noexcept
{
    return plan_;
}

std::size_t FilmstripWidget::itemCount() const noexcept
{
    return items_.size();
}

std::size_t FilmstripWidget::readyItemCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(
        items_.cbegin(),
        items_.cend(),
        [](const FilmstripItem& item) { return item.state == FilmstripItemState::Ready; }));
}

std::size_t FilmstripWidget::failedItemCount() const noexcept
{
    return static_cast<std::size_t>(std::count_if(
        items_.cbegin(),
        items_.cend(),
        [](const FilmstripItem& item) { return item.state == FilmstripItemState::Failed; }));
}

const FilmstripItem* FilmstripWidget::item(const std::size_t index) const noexcept
{
    return index < items_.size() ? &items_[index] : nullptr;
}

QRectF FilmstripWidget::itemRect(const std::size_t index) const noexcept
{
    if (index >= items_.size() || items_.empty()) {
        return {};
    }

    const qreal availableWidth = std::max<qreal>(
        0.0,
        static_cast<qreal>(width()) - 2.0 * kOuterMargin
            - kItemGap * static_cast<qreal>(items_.size() - 1));
    const qreal calculatedWidth = availableWidth / static_cast<qreal>(items_.size());
    const qreal itemWidth = std::clamp(
        calculatedWidth,
        static_cast<qreal>(kMinimumItemWidth),
        static_cast<qreal>(kMaximumItemWidth));
    const qreal left = kOuterMargin
        + static_cast<qreal>(index) * (itemWidth + kItemGap);
    return QRectF(
        left,
        kOuterMargin,
        itemWidth,
        std::max<qreal>(1.0, static_cast<qreal>(height()) - 2.0 * kOuterMargin));
}

QSize FilmstripWidget::preferredThumbnailSize() const noexcept
{
    return QSize(200, 112);
}

QSize FilmstripWidget::sizeHint() const
{
    const auto count = std::max<std::size_t>(items_.size(), plan_.requestedCount);
    const auto maximumInt = static_cast<std::size_t>(std::numeric_limits<int>::max());
    const std::size_t itemPixels = std::min(
        maximumInt,
        count * static_cast<std::size_t>(kPreferredItemWidth));
    const std::size_t gaps = count > 1
        ? (count - 1) * static_cast<std::size_t>(std::lround(kItemGap))
        : 0;
    const std::size_t widthValue = std::min(
        maximumInt,
        itemPixels + gaps + static_cast<std::size_t>(2 * std::lround(kOuterMargin)));
    return QSize(static_cast<int>(widthValue), kPreferredHeight);
}

QSize FilmstripWidget::minimumSizeHint() const
{
    const auto count = std::max<std::size_t>(items_.size(), plan_.requestedCount);
    const auto maximumInt = static_cast<std::size_t>(std::numeric_limits<int>::max());
    const std::size_t itemPixels = std::min(
        maximumInt,
        count * static_cast<std::size_t>(kMinimumItemWidth));
    const std::size_t gaps = count > 1
        ? (count - 1) * static_cast<std::size_t>(std::lround(kItemGap))
        : 0;
    const std::size_t widthValue = std::min(
        maximumInt,
        itemPixels + gaps + static_cast<std::size_t>(2 * std::lround(kOuterMargin)));
    return QSize(static_cast<int>(widthValue), 118);
}

void FilmstripWidget::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.fillRect(rect(), QColor(15, 18, 23));

    if (items_.empty()) {
        painter.setPen(QColor(126, 137, 151));
        painter.drawText(
            QRectF(rect()).adjusted(16.0, 8.0, -16.0, -8.0),
            Qt::AlignCenter | Qt::TextWordWrap,
            emptyMessage_);
        return;
    }

    std::optional<std::size_t> playheadIndex;
    std::uint64_t playheadDistance = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t index = 0; index < items_.size(); ++index) {
        const auto distance = absoluteDistance(itemTimestamp(items_[index]), playheadNanoseconds_);
        if (!playheadIndex || distance < playheadDistance) {
            playheadIndex = index;
            playheadDistance = distance;
        }
    }

    const QFontMetrics metrics(font());
    for (std::size_t index = 0; index < items_.size(); ++index) {
        const auto& item = items_[index];
        const QRectF cell = itemRect(index);
        const bool selected = selectedIndex_ && *selectedIndex_ == index;
        const bool hovered = hoveredIndex_ && *hoveredIndex_ == index;
        const bool current = playheadIndex ? *playheadIndex == index : item.target.current;

        QColor background = hovered ? QColor(35, 43, 54) : QColor(26, 31, 39);
        QColor border = QColor(54, 63, 75);
        qreal borderWidth = 1.0;
        if (current) {
            border = QColor(74, 169, 255);
            borderWidth = 1.5;
        }
        if (selected) {
            border = QColor(255, 196, 77);
            borderWidth = 2.0;
        }

        painter.setBrush(background);
        painter.setPen(QPen(border, borderWidth));
        painter.drawRoundedRect(cell.adjusted(0.5, 0.5, -0.5, -0.5), kItemRadius, kItemRadius);

        const qreal metadataHeight = std::max<qreal>(34.0, metrics.height() * 2.0 + 7.0);
        const QRectF imageArea = cell.adjusted(6.0, 6.0, -6.0, -metadataHeight);
        painter.fillRect(imageArea, QColor(7, 9, 12));

        if (item.state == FilmstripItemState::Ready && item.frame
            && !item.frame->image.isNull()) {
            const QRectF imageRect = fittedImageRect(imageArea, item.frame->image.size());
            painter.drawImage(imageRect, item.frame->image);

            QString badge = pictureTypeText(item.frame->pictureType);
            if (item.frame->keyFrame) {
                badge += QStringLiteral(" K");
            }
            const QRect badgeBounds = metrics.boundingRect(badge).adjusted(-4, -2, 4, 2);
            QRectF badgeRect(
                imageArea.left() + 4.0,
                imageArea.top() + 4.0,
                static_cast<qreal>(badgeBounds.width()),
                static_cast<qreal>(badgeBounds.height()));
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(8, 11, 15, 210));
            painter.drawRoundedRect(badgeRect, 3.0, 3.0);
            painter.setPen(QColor(236, 241, 248));
            painter.drawText(badgeRect, Qt::AlignCenter, badge);

            if (item.frame->motionScore || item.frame->similarityScore) {
                const QString analysis = QStringLiteral("M %1  S %2")
                    .arg(item.frame->motionScore
                            ? QStringLiteral("%1%").arg(
                                  std::clamp(*item.frame->motionScore, 0.0F, 1.0F) * 100.0F,
                                  0,
                                  'f',
                                  0)
                            : QStringLiteral("—"))
                    .arg(item.frame->similarityScore
                            ? QStringLiteral("%1%").arg(
                                  std::clamp(*item.frame->similarityScore, 0.0F, 1.0F) * 100.0F,
                                  0,
                                  'f',
                                  0)
                            : QStringLiteral("—"));
                const QRect analysisBounds = metrics.boundingRect(analysis).adjusted(-4, -2, 4, 2);
                QRectF analysisRect(
                    imageArea.left() + 4.0,
                    imageArea.bottom() - static_cast<qreal>(analysisBounds.height()) - 4.0,
                    static_cast<qreal>(analysisBounds.width()),
                    static_cast<qreal>(analysisBounds.height()));
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(8, 11, 15, 210));
                painter.drawRoundedRect(analysisRect, 3.0, 3.0);
                painter.setPen(QColor(143, 205, 255));
                painter.drawText(analysisRect, Qt::AlignCenter, analysis);
            }
        } else {
            painter.setPen(item.state == FilmstripItemState::Failed
                    ? QColor(224, 113, 120)
                    : QColor(116, 128, 143));
            painter.drawText(
                imageArea.adjusted(5.0, 5.0, -5.0, -5.0),
                Qt::AlignCenter | Qt::TextWordWrap,
                item.state == FilmstripItemState::Failed
                    ? tr("Unavailable")
                    : tr("Loading…"));
        }

        const qint64 timestamp = itemTimestamp(item);
        const qint64 presentationIndex = itemPresentationIndex(item);
        const QString timeText = metrics.elidedText(
            formatTime(timestamp),
            Qt::ElideRight,
            static_cast<int>(cell.width() - 12.0));
        const QString frameText = presentationIndex >= 0
            ? tr("Frame %1").arg(presentationIndex)
            : tr("Frame —");

        painter.setPen(QColor(220, 226, 235));
        painter.drawText(
            QRectF(cell.left() + 6.0, imageArea.bottom() + 3.0, cell.width() - 12.0, metrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            timeText);
        painter.setPen(QColor(135, 147, 163));
        painter.drawText(
            QRectF(cell.left() + 6.0, imageArea.bottom() + 3.0 + metrics.height(), cell.width() - 12.0, metrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            metrics.elidedText(frameText, Qt::ElideRight, static_cast<int>(cell.width() - 12.0)));
    }

    if (hasFocus()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(75, 169, 255, 120), 1.0));
        painter.drawRoundedRect(QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0), 5.0, 5.0);
    }
}

void FilmstripWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    setFocus(Qt::MouseFocusReason);
    pressedIndex_ = indexAt(event->position());
    if (pressedIndex_) {
        selectedIndex_ = pressedIndex_;
        update();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void FilmstripWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && pressedIndex_) {
        const auto released = indexAt(event->position());
        const auto pressed = pressedIndex_;
        pressedIndex_.reset();
        if (released && pressed == released) {
            activateItem(*released);
        }
        event->accept();
        return;
    }
    pressedIndex_.reset();
    QWidget::mouseReleaseEvent(event);
}

void FilmstripWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        if (const auto index = indexAt(event->position())) {
            selectedIndex_ = index;
            inspectItem(*index);
            update();
            event->accept();
            return;
        }
    }
    QWidget::mouseDoubleClickEvent(event);
}

void FilmstripWidget::mouseMoveEvent(QMouseEvent* event)
{
    const auto index = indexAt(event->position());
    if (index != hoveredIndex_) {
        hoveredIndex_ = index;
        updateItemToolTip(index);
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void FilmstripWidget::leaveEvent(QEvent* event)
{
    if (hoveredIndex_) {
        hoveredIndex_.reset();
        setToolTip({});
        update();
    }
    QWidget::leaveEvent(event);
}

void FilmstripWidget::keyPressEvent(QKeyEvent* event)
{
    if (items_.empty()) {
        QWidget::keyPressEvent(event);
        return;
    }

    std::size_t selected = selectedIndex_.value_or(0);
    if (event->key() == Qt::Key_Left) {
        selectedIndex_ = selected > 0 ? selected - 1 : 0;
        update();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Right) {
        selectedIndex_ = std::min(selected + 1, items_.size() - 1);
        update();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter
        || event->key() == Qt::Key_Space) {
        activateItem(selected);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

std::optional<std::size_t> FilmstripWidget::indexAt(const QPointF position) const noexcept
{
    for (std::size_t index = 0; index < items_.size(); ++index) {
        if (itemRect(index).contains(position)) {
            return index;
        }
    }
    return std::nullopt;
}

qint64 FilmstripWidget::itemTimestamp(const FilmstripItem& item) const noexcept
{
    const auto time = item.frame
        ? item.frame->presentationTime
        : item.target.requestedTime;
    return static_cast<qint64>(std::max(time, media::MediaTime::zero()).count());
}

qint64 FilmstripWidget::itemPresentationIndex(const FilmstripItem& item) const noexcept
{
    return item.frame
        ? static_cast<qint64>(item.frame->presentationIndex)
        : static_cast<qint64>(item.target.presentationIndexHint);
}

void FilmstripWidget::activateItem(const std::size_t index)
{
    if (index >= items_.size()) {
        return;
    }
    selectedIndex_ = index;
    emit seekRequested(itemTimestamp(items_[index]));
    update();
}

void FilmstripWidget::inspectItem(const std::size_t index)
{
    if (index >= items_.size()) {
        return;
    }
    emit frameInspectorRequested(
        itemTimestamp(items_[index]),
        itemPresentationIndex(items_[index]));
}

void FilmstripWidget::updateItemToolTip(const std::optional<std::size_t> index)
{
    if (!index || *index >= items_.size()) {
        setToolTip({});
        return;
    }

    const auto& item = items_[*index];
    QString text = tr("Requested: %1").arg(
        formatTime(static_cast<qint64>(item.target.requestedTime.count())));
    if (item.frame) {
        text += tr("\nDecoded: %1\nFrame: %2\nPTS: %3\nDTS: %4\nType: %5\nKeyframe: %6")
            .arg(formatTime(static_cast<qint64>(item.frame->presentationTime.count())))
            .arg(item.frame->presentationIndex >= 0
                    ? QString::number(item.frame->presentationIndex)
                    : QStringLiteral("—"))
            .arg(item.frame->pts != AV_NOPTS_VALUE
                    ? QString::number(item.frame->pts)
                    : QStringLiteral("N/A"))
            .arg(item.frame->dts != AV_NOPTS_VALUE
                    ? QString::number(item.frame->dts)
                    : QStringLiteral("N/A"))
            .arg(pictureTypeText(item.frame->pictureType))
            .arg(item.frame->keyFrame ? tr("yes") : tr("no"));
        if (item.frame->motionScore || item.frame->similarityScore) {
            text += tr("\nMotion: %1 | Similarity: %2")
                .arg(item.frame->motionScore
                        ? QStringLiteral("%1%").arg(
                              std::clamp(*item.frame->motionScore, 0.0F, 1.0F) * 100.0F,
                              0,
                              'f',
                              1)
                        : tr("not analyzed"))
                .arg(item.frame->similarityScore
                        ? QStringLiteral("%1%").arg(
                              std::clamp(*item.frame->similarityScore, 0.0F, 1.0F) * 100.0F,
                              0,
                              'f',
                              1)
                        : tr("not analyzed"));
        }
    } else if (item.state == FilmstripItemState::Failed) {
        text += QStringLiteral("\n") + item.failureDetail;
    }
    setToolTip(text);
}

} // namespace vidscope::widgets
