#include "widgets/HoverPreviewController.h"

#include "timeline/TimelineWidget.h"
#include "widgets/HoverPreviewPopup.h"

#include <algorithm>
#include <utility>

namespace vidscope::widgets {
namespace {

constexpr QSize kMaximumHoverPreviewSize{640, 360};

} // namespace

HoverPreviewController::HoverPreviewController(
    timeline::TimelineWidget* timeline,
    thumbnails::ThumbnailManager* manager,
    QWidget* anchorWindow,
    HoverPreviewConfig config,
    QObject* parent)
    : HoverPreviewController(
          timeline,
          manager,
          nullptr,
          anchorWindow,
          std::move(config),
          parent)
{
}

HoverPreviewController::HoverPreviewController(
    timeline::TimelineWidget* timeline,
    thumbnails::ThumbnailManager* manager,
    analysis::AnalysisManager* analysisManager,
    QWidget* anchorWindow,
    HoverPreviewConfig config,
    QObject* parent)
    : QObject(parent)
    , timeline_(timeline)
    , manager_(manager)
    , analysisManager_(analysisManager)
    , popup_(new HoverPreviewPopup(anchorWindow))
    , config_(std::move(config))
{
    setObjectName(QStringLiteral("hoverPreviewController"));
    popup_->setObjectName(QStringLiteral("hoverPreviewPopup"));
    config_.debounceMilliseconds = std::clamp(config_.debounceMilliseconds, 0, 500);
    if (!config_.targetSize.isValid()
        || config_.targetSize.width() <= 0
        || config_.targetSize.height() <= 0) {
        config_.targetSize = QSize(320, 180);
    }
    config_.targetSize = config_.targetSize.boundedTo(kMaximumHoverPreviewSize);
    popup_->setPreviewSize(config_.targetSize);

    debounceTimer_.setSingleShot(true);
    debounceTimer_.setInterval(config_.debounceMilliseconds);
    connect(&debounceTimer_, &QTimer::timeout, this, &HoverPreviewController::dispatchRequest);
    connect(
        timeline_,
        &timeline::TimelineWidget::hoverPreviewChanged,
        this,
        &HoverPreviewController::handleHover);
    connect(
        manager_,
        &thumbnails::ThumbnailManager::previewReady,
        this,
        &HoverPreviewController::handlePreview);
    connect(
        manager_,
        &thumbnails::ThumbnailManager::previewFailed,
        this,
        &HoverPreviewController::handleFailure);
    if (analysisManager_ != nullptr) {
        connect(
            analysisManager_,
            &analysis::AnalysisManager::samplesAvailable,
            this,
            [this](qint64 start, qint64 end, quint64) {
                handleAnalysisSamples(start, end);
            });
    }
}

HoverPreviewController::~HoverPreviewController()
{
    clear();
    delete popup_;
}

void HoverPreviewController::clear()
{
    debounceTimer_.stop();
    hoverActive_ = false;
    currentGeneration_ = 0;
    manager_->cancelHoverPreview();
    popup_->dismiss();
}

HoverPreviewPopup* HoverPreviewController::popup() const noexcept
{
    return popup_;
}

void HoverPreviewController::handleHover(
    qint64 timestampNanoseconds,
    qint64 presentationIndex,
    QPoint globalPosition,
    bool active)
{
    if (!active) {
        clear();
        return;
    }

    timestampNanoseconds = std::max<qint64>(0, timestampNanoseconds);
    if (hoverActive_ && pendingTimestampNanoseconds_ == timestampNanoseconds) {
        pendingPresentationIndex_ = presentationIndex;
        pendingGlobalPosition_ = globalPosition;
        popup_->updateCursorPosition(globalPosition);
        return;
    }

    hoverActive_ = true;
    pendingTimestampNanoseconds_ = timestampNanoseconds;
    pendingPresentationIndex_ = presentationIndex;
    pendingGlobalPosition_ = globalPosition;
    currentGeneration_ = 0;
    manager_->cancelHoverPreview();
    popup_->showPending(0, timestampNanoseconds, presentationIndex, globalPosition);
    debounceTimer_.start();
}

void HoverPreviewController::dispatchRequest()
{
    if (!hoverActive_) {
        return;
    }

    const auto generation = manager_->requestPreview(
        pendingTimestampNanoseconds_,
        config_.targetSize,
        thumbnails::ThumbnailPriority::HoverPreview,
        pendingPresentationIndex_);
    if (generation == 0) {
        popup_->dismiss();
        currentGeneration_ = 0;
        return;
    }

    currentGeneration_ = generation;
    popup_->showPending(
        generation,
        pendingTimestampNanoseconds_,
        pendingPresentationIndex_,
        pendingGlobalPosition_);
}

void HoverPreviewController::handlePreview(const thumbnails::ThumbnailResult& result)
{
    if (!hoverActive_ || currentGeneration_ == 0
        || result.request.generation != currentGeneration_) {
        return;
    }
    auto enriched = result;
    if (analysisManager_ != nullptr) {
        const qint64 timestamp = enriched.frame.presentationTime == media::kNoMediaTime
            ? static_cast<qint64>(enriched.request.timestamp.count())
            : static_cast<qint64>(enriched.frame.presentationTime.count());
        if (const auto sample = analysisManager_->sampleFor(
                timestamp,
                enriched.frame.presentationIndex)) {
            enriched.frame.motionScore = sample->motion;
            enriched.frame.similarityScore = sample->similarity;
        }
    }
    popup_->showPreview(enriched, pendingGlobalPosition_);
}

void HoverPreviewController::handleAnalysisSamples(
    const qint64 startNanoseconds,
    const qint64 endNanoseconds)
{
    if (!hoverActive_ || !popup_->hasPreviewImage() || analysisManager_ == nullptr) {
        return;
    }
    const qint64 timestamp = popup_->displayedTimestampNanoseconds();
    if (timestamp < std::min(startNanoseconds, endNanoseconds)
        || timestamp > std::max(startNanoseconds, endNanoseconds)) {
        return;
    }
    if (const auto sample = analysisManager_->sampleFor(
            timestamp,
            pendingPresentationIndex_)) {
        popup_->setAnalysisScores(sample->motion, sample->similarity);
    }
}

void HoverPreviewController::handleFailure(
    thumbnails::ThumbnailGeneration generation,
    const QString& detail)
{
    if (!hoverActive_ || generation == 0 || generation != currentGeneration_) {
        return;
    }
    popup_->showFailure(
        generation,
        pendingTimestampNanoseconds_,
        pendingPresentationIndex_,
        detail,
        pendingGlobalPosition_);
}

} // namespace vidscope::widgets
