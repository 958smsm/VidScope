#include "widgets/AnalysisResultsPanel.h"

#include "thumbnails/ThumbnailManager.h"

#include <QtCore/QSignalBlocker>
#include <QtGui/QIcon>
#include <QtGui/QPixmap>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <chrono>

namespace vidscope::widgets {
namespace {

constexpr int kTimestampRole = Qt::UserRole;
constexpr std::size_t kMaximumSceneThumbnails = 48;

[[nodiscard]] QString formatTime(media::MediaTime time)
{
    constexpr qint64 nanosecondsPerMillisecond = 1'000'000;
    constexpr qint64 millisecondsPerSecond = 1'000;
    constexpr qint64 secondsPerMinute = 60;
    constexpr qint64 minutesPerHour = 60;
    const qint64 totalMilliseconds = std::max<qint64>(
        0,
        static_cast<qint64>(time.count()) / nanosecondsPerMillisecond);
    const qint64 milliseconds = totalMilliseconds % millisecondsPerSecond;
    const qint64 totalSeconds = totalMilliseconds / millisecondsPerSecond;
    const qint64 seconds = totalSeconds % secondsPerMinute;
    const qint64 totalMinutes = totalSeconds / secondsPerMinute;
    const qint64 minutes = totalMinutes % minutesPerHour;
    const qint64 hours = totalMinutes / minutesPerHour;
    return QStringLiteral("%1:%2:%3.%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(milliseconds, 3, 10, QLatin1Char('0'));
}

[[nodiscard]] QString frameRange(const analysis::DetectionResult& result)
{
    if (result.firstFrame >= 0 && result.lastFrame >= 0) {
        return AnalysisResultsPanel::tr("Frames %1–%2")
            .arg(result.firstFrame)
            .arg(result.lastFrame);
    }
    return AnalysisResultsPanel::tr("%1–%2")
        .arg(formatTime(result.start))
        .arg(formatTime(result.end));
}

[[nodiscard]] QString detectionName(const analysis::DetectionKind kind)
{
    switch (kind) {
    case analysis::DetectionKind::SceneChange:
        return AnalysisResultsPanel::tr("Scene");
    case analysis::DetectionKind::ExactDuplicate:
        return AnalysisResultsPanel::tr("Exact");
    case analysis::DetectionKind::NearDuplicate:
        return AnalysisResultsPanel::tr("Near");
    case analysis::DetectionKind::RepeatedSection:
        return AnalysisResultsPanel::tr("Repeated");
    case analysis::DetectionKind::Freeze:
        return AnalysisResultsPanel::tr("Freeze");
    }
    return {};
}

[[nodiscard]] QString percent(const float value)
{
    return QStringLiteral("%1%").arg(
        static_cast<double>(std::clamp(value, 0.0F, 1.0F)) * 100.0,
        0,
        'f',
        1);
}

} // namespace

AnalysisResultsPanel::AnalysisResultsPanel(QWidget* parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("analysisResultsPanel"));
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(7);

    summary_ = new QLabel(tr("No detection results"), this);
    summary_->setObjectName(QStringLiteral("detectionSummary"));
    summary_->setWordWrap(true);
    root->addWidget(summary_);

    auto* controls = new QFormLayout;
    controls->setContentsMargins(0, 0, 0, 0);
    sceneThreshold_ = new QDoubleSpinBox(this);
    sceneThreshold_->setObjectName(QStringLiteral("sceneThreshold"));
    sceneThreshold_->setRange(0.0, 1.0);
    sceneThreshold_->setDecimals(3);
    sceneThreshold_->setSingleStep(0.025);
    controls->addRow(tr("Scene threshold"), sceneThreshold_);

    duplicateThreshold_ = new QDoubleSpinBox(this);
    duplicateThreshold_->setObjectName(QStringLiteral("duplicateThreshold"));
    duplicateThreshold_->setRange(0.0, 1.0);
    duplicateThreshold_->setDecimals(3);
    duplicateThreshold_->setSingleStep(0.005);
    controls->addRow(tr("Near duplicate"), duplicateThreshold_);

    freezeThreshold_ = new QDoubleSpinBox(this);
    freezeThreshold_->setObjectName(QStringLiteral("freezeThreshold"));
    freezeThreshold_->setRange(0.0, 1.0);
    freezeThreshold_->setDecimals(3);
    freezeThreshold_->setSingleStep(0.001);
    controls->addRow(tr("Freeze similarity"), freezeThreshold_);

    freezeDuration_ = new QSpinBox(this);
    freezeDuration_->setObjectName(QStringLiteral("freezeDuration"));
    freezeDuration_->setRange(0, 600'000);
    freezeDuration_->setSuffix(tr(" ms"));
    controls->addRow(tr("Minimum freeze"), freezeDuration_);
    root->addLayout(controls);

    reanalyze_ = new QPushButton(tr("Reanalyze Detections"), this);
    reanalyze_->setObjectName(QStringLiteral("reanalyzeDetections"));
    connect(reanalyze_, &QPushButton::clicked, this, [this] {
        config_ = configFromControls();
        emit reanalyzeRequested(config_);
    });
    root->addWidget(reanalyze_);

    tabs_ = new QTabWidget(this);
    tabs_->setObjectName(QStringLiteral("detectionTabs"));
    scenes_ = new QTreeWidget(tabs_);
    scenes_->setObjectName(QStringLiteral("sceneResults"));
    scenes_->setHeaderLabels({tr("Scene"), tr("Time"), tr("Score")});
    scenes_->setIconSize(QSize(96, 54));
    configureTree(scenes_);
    duplicates_ = new QTreeWidget(tabs_);
    duplicates_->setObjectName(QStringLiteral("duplicateResults"));
    duplicates_->setHeaderLabels({tr("Type"), tr("Range"), tr("Score"), tr("Match")});
    configureTree(duplicates_);
    freezes_ = new QTreeWidget(tabs_);
    freezes_->setObjectName(QStringLiteral("freezeResults"));
    freezes_->setHeaderLabels({tr("Frames"), tr("Range"), tr("Similarity")});
    configureTree(freezes_);
    tabs_->addTab(scenes_, tr("Scenes"));
    tabs_->addTab(duplicates_, tr("Duplicates"));
    tabs_->addTab(freezes_, tr("Freezes"));
    root->addWidget(tabs_, 1);

    for (auto* tree : {scenes_, duplicates_, freezes_}) {
        connect(tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
            activateItem(item);
        });
    }
    setDetectionConfig(config_);
}

void AnalysisResultsPanel::configureTree(QTreeWidget* tree)
{
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->header()->setStretchLastSection(true);
    tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
}

void AnalysisResultsPanel::setThumbnailManager(thumbnails::ThumbnailManager* manager)
{
    if (thumbnailManager_ == manager) {
        return;
    }
    if (thumbnailManager_) {
        thumbnailManager_->cancelRequests(thumbnails::ThumbnailPriority::BackgroundPrecache);
        disconnect(thumbnailManager_, nullptr, this, nullptr);
    }
    pendingThumbnails_.clear();
    thumbnailManager_ = manager;
    if (thumbnailManager_) {
        connect(
            thumbnailManager_,
            &thumbnails::ThumbnailManager::previewReady,
            this,
            &AnalysisResultsPanel::handleThumbnail);
        connect(
            thumbnailManager_,
            &thumbnails::ThumbnailManager::previewFailed,
            this,
            [this](const thumbnails::ThumbnailGeneration generation, const QString&) {
                pendingThumbnails_.remove(generation);
            });
        connect(
            thumbnailManager_,
            &thumbnails::ThumbnailManager::previewCancelled,
            this,
            [this](const thumbnails::ThumbnailGeneration generation) {
                pendingThumbnails_.remove(generation);
            });
    }
    requestSceneThumbnails();
}

void AnalysisResultsPanel::setDetectionConfig(const analysis::DetectionConfig& config)
{
    config_ = analysis::DetectionEngine::normalized(config);
    const QSignalBlocker sceneBlocker(sceneThreshold_);
    const QSignalBlocker duplicateBlocker(duplicateThreshold_);
    const QSignalBlocker freezeBlocker(freezeThreshold_);
    const QSignalBlocker durationBlocker(freezeDuration_);
    sceneThreshold_->setValue(config_.sceneThreshold);
    duplicateThreshold_->setValue(config_.nearDuplicateThreshold);
    freezeThreshold_->setValue(config_.freezeThreshold);
    freezeDuration_->setValue(static_cast<int>(std::clamp<qint64>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            config_.minimumFreezeDuration).count(),
        0,
        600'000)));
}

analysis::DetectionConfig AnalysisResultsPanel::configFromControls() const
{
    auto config = config_;
    config.sceneThreshold = static_cast<float>(sceneThreshold_->value());
    config.nearDuplicateThreshold = static_cast<float>(duplicateThreshold_->value());
    config.freezeThreshold = static_cast<float>(freezeThreshold_->value());
    config.minimumFreezeDuration = std::chrono::milliseconds(freezeDuration_->value());
    return analysis::DetectionEngine::normalized(config);
}

void AnalysisResultsPanel::setResults(const analysis::DetectionResults& results)
{
    if (thumbnailManager_) {
        thumbnailManager_->cancelRequests(thumbnails::ThumbnailPriority::BackgroundPrecache);
    }
    pendingThumbnails_.clear();
    results_ = results;
    scenes_->clear();
    duplicates_->clear();
    freezes_->clear();

    std::size_t sceneNumber = 1;
    for (const auto& scene : results_.scenes) {
        auto* item = new QTreeWidgetItem(scenes_);
        item->setText(0, tr("Scene %1").arg(sceneNumber++));
        item->setText(1, formatTime(scene.start));
        item->setText(2, percent(scene.score));
        item->setData(0, kTimestampRole, QVariant::fromValue<qint64>(
            static_cast<qint64>(scene.start.count())));
        item->setToolTip(0, frameRange(scene));
    }

    for (const auto& duplicate : results_.duplicates) {
        auto* item = new QTreeWidgetItem(duplicates_);
        item->setText(0, detectionName(duplicate.kind));
        item->setText(1, frameRange(duplicate));
        item->setText(2, percent(duplicate.score));
        if (duplicate.matchingStart && duplicate.matchingEnd) {
            item->setText(
                3,
                tr("%1–%2")
                    .arg(formatTime(*duplicate.matchingStart))
                    .arg(formatTime(*duplicate.matchingEnd)));
        }
        item->setData(0, kTimestampRole, QVariant::fromValue<qint64>(
            static_cast<qint64>(duplicate.start.count())));
        item->setToolTip(
            1,
            tr("%1 | %2 frames | duration %3")
                .arg(frameRange(duplicate))
                .arg(duplicate.frameCount)
                .arg(formatTime(duplicate.end - duplicate.start)));
    }

    for (const auto& freeze : results_.freezes) {
        auto* item = new QTreeWidgetItem(freezes_);
        item->setText(0, frameRange(freeze));
        item->setText(
            1,
            tr("%1–%2").arg(formatTime(freeze.start)).arg(formatTime(freeze.end)));
        item->setText(2, percent(freeze.score));
        item->setData(0, kTimestampRole, QVariant::fromValue<qint64>(
            static_cast<qint64>(freeze.start.count())));
    }

    tabs_->setTabText(0, tr("Scenes (%1)").arg(results_.scenes.size()));
    tabs_->setTabText(1, tr("Duplicates (%1)").arg(results_.duplicates.size()));
    tabs_->setTabText(2, tr("Freezes (%1)").arg(results_.freezes.size()));
    summary_->setText(
        tr("%1 samples | %2 scenes | %3 duplicate ranges | %4 freezes")
            .arg(results_.analyzedSamples)
            .arg(results_.scenes.size())
            .arg(results_.duplicates.size())
            .arg(results_.freezes.size()));
    requestSceneThumbnails();
}

void AnalysisResultsPanel::clearResults()
{
    if (thumbnailManager_) {
        thumbnailManager_->cancelRequests(thumbnails::ThumbnailPriority::BackgroundPrecache);
    }
    pendingThumbnails_.clear();
    results_ = {};
    scenes_->clear();
    duplicates_->clear();
    freezes_->clear();
    tabs_->setTabText(0, tr("Scenes"));
    tabs_->setTabText(1, tr("Duplicates"));
    tabs_->setTabText(2, tr("Freezes"));
    summary_->setText(tr("No detection results"));
}

void AnalysisResultsPanel::activateItem(QTreeWidgetItem* item)
{
    if (item == nullptr) {
        return;
    }
    bool valid = false;
    const qint64 timestamp = item->data(0, kTimestampRole).toLongLong(&valid);
    if (valid) {
        emit seekRequested(timestamp);
    }
}

void AnalysisResultsPanel::requestSceneThumbnails()
{
    if (!thumbnailManager_) {
        return;
    }
    const std::size_t count = std::min<std::size_t>(
        results_.scenes.size(),
        kMaximumSceneThumbnails);
    for (std::size_t index = 0; index < count; ++index) {
        auto* item = scenes_->topLevelItem(static_cast<int>(index));
        if (item == nullptr) {
            continue;
        }
        const auto& scene = results_.scenes[index];
        const auto generation = thumbnailManager_->requestPreview(
            static_cast<qint64>(scene.start.count()),
            QSize(128, 72),
            thumbnails::ThumbnailPriority::BackgroundPrecache,
            scene.firstFrame);
        pendingThumbnails_.insert(generation, item);
    }
}

void AnalysisResultsPanel::handleThumbnail(const thumbnails::ThumbnailResult& result)
{
    const auto found = pendingThumbnails_.find(result.request.generation);
    if (found == pendingThumbnails_.end()) {
        return;
    }
    QTreeWidgetItem* item = found.value();
    pendingThumbnails_.erase(found);
    if (item != nullptr && !result.frame.image.isNull()) {
        item->setIcon(0, QIcon(QPixmap::fromImage(result.frame.image)));
    }
}

} // namespace vidscope::widgets
