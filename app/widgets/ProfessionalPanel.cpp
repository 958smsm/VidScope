#include "widgets/ProfessionalPanel.h"

#include <QtCore/QVariant>
#include <QtGui/QFontDatabase>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <chrono>

namespace vidscope::widgets {
namespace {

constexpr int kTimestampRole = Qt::UserRole;
constexpr int kMarkerIdRole = Qt::UserRole + 1;

[[nodiscard]] QString formatTime(const media::MediaTime time)
{
    const auto totalMilliseconds = std::max<qint64>(
        0,
        std::chrono::duration_cast<std::chrono::milliseconds>(time).count());
    const auto milliseconds = totalMilliseconds % 1'000;
    const auto totalSeconds = totalMilliseconds / 1'000;
    const auto seconds = totalSeconds % 60;
    const auto totalMinutes = totalSeconds / 60;
    const auto minutes = totalMinutes % 60;
    const auto hours = totalMinutes / 60;
    return QStringLiteral("%1:%2:%3.%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(milliseconds, 3, 10, QLatin1Char('0'));
}

[[nodiscard]] QString markerKindName(const timeline::TimelineMarkerKind kind)
{
    switch (kind) {
    case timeline::TimelineMarkerKind::Keyframe:
        return ProfessionalPanel::tr("Keyframe");
    case timeline::TimelineMarkerKind::Scene:
        return ProfessionalPanel::tr("Scene");
    case timeline::TimelineMarkerKind::Chapter:
        return ProfessionalPanel::tr("Chapter");
    case timeline::TimelineMarkerKind::Bookmark:
        return ProfessionalPanel::tr("Bookmark");
    }
    return {};
}

[[nodiscard]] double cacheHitPercent(const playback::FrameCacheStats& stats)
{
    const auto requests = stats.hits + stats.misses;
    return requests == 0
        ? 0.0
        : static_cast<double>(stats.hits) * 100.0
            / static_cast<double>(requests);
}

} // namespace

ProfessionalPanel::ProfessionalPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("professionalPanel"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("professionalTabs"));
    history_ = new QTreeWidget(tabs_);
    history_->setObjectName(QStringLiteral("frameHistory"));
    history_->setHeaderLabels({tr("Frame"), tr("Time"), tr("Type")});
    history_->setRootIsDecorated(false);
    history_->setAlternatingRowColors(true);
    history_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    history_->header()->setStretchLastSection(true);
    tabs_->addTab(history_, tr("History"));

    auto* markerPage = new QWidget(tabs_);
    auto* markerLayout = new QVBoxLayout(markerPage);
    markerLayout->setContentsMargins(0, 0, 0, 0);
    markers_ = new QTreeWidget(markerPage);
    markers_->setObjectName(QStringLiteral("markerNotes"));
    markers_->setHeaderLabels(
        {tr("Type"), tr("Category"), tr("Time"), tr("Label"), tr("Note")});
    markers_->setRootIsDecorated(false);
    markers_->setAlternatingRowColors(true);
    markers_->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    markers_->header()->setStretchLastSection(true);
    markerLayout->addWidget(markers_, 1);
    auto* markerButtons = new QHBoxLayout;
    markerButtons->addStretch(1);
    editMarker_ = new QPushButton(tr("Edit"), markerPage);
    editMarker_->setObjectName(QStringLiteral("editMarker"));
    deleteMarker_ = new QPushButton(tr("Delete"), markerPage);
    deleteMarker_->setObjectName(QStringLiteral("deleteMarker"));
    markerButtons->addWidget(editMarker_);
    markerButtons->addWidget(deleteMarker_);
    markerLayout->addLayout(markerButtons);
    tabs_->addTab(markerPage, tr("Markers"));

    diagnostics_ = new QPlainTextEdit(tabs_);
    diagnostics_->setObjectName(QStringLiteral("playbackDiagnostics"));
    diagnostics_->setReadOnly(true);
    diagnostics_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    tabs_->addTab(diagnostics_, tr("Diagnostics"));
    root->addWidget(tabs_);

    connect(
        history_,
        &QTreeWidget::itemActivated,
        this,
        [this](QTreeWidgetItem* item, int) { activateTimestampItem(item); });
    connect(
        markers_,
        &QTreeWidget::itemActivated,
        this,
        [this](QTreeWidgetItem* item, int) { activateTimestampItem(item); });
    connect(editMarker_, &QPushButton::clicked, this, [this] {
        if (const auto id = selectedMarkerId()) {
            emit editMarkerRequested(*id);
        }
    });
    connect(deleteMarker_, &QPushButton::clicked, this, [this] {
        if (const auto id = selectedMarkerId()) {
            emit deleteMarkerRequested(*id);
        }
    });
}

void ProfessionalPanel::setHistory(
    const std::span<const inspection::FrameHistoryEntry> entries,
    const std::optional<std::size_t> currentIndex)
{
    history_->clear();
    for (std::size_t reverseIndex = entries.size(); reverseIndex > 0; --reverseIndex) {
        const std::size_t index = reverseIndex - 1;
        const auto& entry = entries[index];
        auto* item = new QTreeWidgetItem(history_);
        item->setText(
            0,
            entry.id.presentationIndex >= 0
                ? QString::number(entry.id.presentationIndex)
                : tr("Unknown"));
        item->setText(1, formatTime(entry.time));
        item->setText(
            2,
            entry.keyFrame
                ? tr("%1 key").arg(QString::fromLatin1(media::pictureTypeName(entry.pictureType)))
                : QString::fromLatin1(media::pictureTypeName(entry.pictureType)));
        item->setData(
            0,
            kTimestampRole,
            QVariant::fromValue<qint64>(static_cast<qint64>(entry.time.count())));
        if (currentIndex && *currentIndex == index) {
            item->setText(0, QStringLiteral("▶ ") + item->text(0));
            QFont font = item->font(0);
            font.setBold(true);
            for (int column = 0; column < history_->columnCount(); ++column) {
                item->setFont(column, font);
            }
        }
    }
    tabs_->setTabText(0, tr("History (%1)").arg(entries.size()));
}

void ProfessionalPanel::setMarkers(
    const std::span<const timeline::TimelineMarker> markers)
{
    markers_->clear();
    for (const auto& marker : markers) {
        auto* item = new QTreeWidgetItem(markers_);
        item->setText(0, markerKindName(marker.kind));
        item->setText(1, marker.category);
        item->setText(2, formatTime(marker.time));
        item->setText(3, marker.label);
        item->setText(4, marker.note);
        item->setData(
            0,
            kTimestampRole,
            QVariant::fromValue<qint64>(static_cast<qint64>(marker.time.count())));
        item->setData(
            0,
            kMarkerIdRole,
            QVariant::fromValue<qulonglong>(marker.id));
    }
    tabs_->setTabText(1, tr("Markers (%1)").arg(markers.size()));
}

void ProfessionalPanel::setDiagnostics(
    const playback::PlaybackDiagnostics& diagnostics)
{
    const QString device = diagnostics.hardwareDecodeActive
        ? (diagnostics.hardwareDevice.isEmpty()
              ? tr("active (device unknown)")
              : tr("active (%1)").arg(diagnostics.hardwareDevice))
        : tr("software");
    diagnostics_->setPlainText(
        tr("Decoder rate       %1 fps\n"
           "Last seek         %2 ms\n"
           "Buffered frames   %3\n"
           "Pending commands  %4\n"
           "Frame cache       %5 entries / %6 MiB\n"
           "Cache hit rate    %7%\n"
           "Cache evictions   %8\n"
           "GUI deliveries    %9\n"
           "Dropped delivery  %10\n"
           "GPU decode        %11\n"
           "GPU utilization   unavailable from active backend")
            .arg(diagnostics.decodeFramesPerSecond, 0, 'f', 1)
            .arg(static_cast<double>(diagnostics.seekMicroseconds) / 1'000.0, 0, 'f', 1)
            .arg(diagnostics.bufferedFrames)
            .arg(diagnostics.pendingCommands)
            .arg(diagnostics.frameCache.frameCount)
            .arg(
                static_cast<double>(diagnostics.frameCache.bytes)
                    / (1024.0 * 1024.0),
                0,
                'f',
                1)
            .arg(cacheHitPercent(diagnostics.frameCache), 0, 'f', 1)
            .arg(diagnostics.frameCache.evictions)
            .arg(diagnostics.deliveredFrames)
            .arg(diagnostics.droppedFrameDeliveries)
            .arg(device));
}

void ProfessionalPanel::clear()
{
    history_->clear();
    markers_->clear();
    diagnostics_->clear();
    tabs_->setTabText(0, tr("History"));
    tabs_->setTabText(1, tr("Markers"));
}

void ProfessionalPanel::activateTimestampItem(QTreeWidgetItem* item)
{
    if (item == nullptr) {
        return;
    }
    bool valid = false;
    const auto timestamp = item->data(0, kTimestampRole).toLongLong(&valid);
    if (valid) {
        emit seekRequested(timestamp);
    }
}

std::optional<quint64> ProfessionalPanel::selectedMarkerId() const
{
    const auto selected = markers_->selectedItems();
    if (selected.isEmpty()) {
        return std::nullopt;
    }
    bool valid = false;
    const auto id = selected.front()->data(0, kMarkerIdRole).toULongLong(&valid);
    return valid ? std::optional<quint64>{id} : std::nullopt;
}

} // namespace vidscope::widgets
