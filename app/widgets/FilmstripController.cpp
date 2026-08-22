#include "widgets/FilmstripController.h"

#include "timeline/TimelineWidget.h"
#include "widgets/FilmstripWidget.h"

#include <QtCore/QObject>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

namespace vidscope::widgets {
namespace {

constexpr QSize kMaximumFilmstripThumbnailSize{640, 360};

[[nodiscard]] std::uint64_t absoluteDistance(qint64 left, qint64 right) noexcept
{
    const auto unsignedLeft = static_cast<std::uint64_t>(std::max<qint64>(0, left));
    const auto unsignedRight = static_cast<std::uint64_t>(std::max<qint64>(0, right));
    return unsignedLeft >= unsignedRight
        ? unsignedLeft - unsignedRight
        : unsignedRight - unsignedLeft;
}

} // namespace

FilmstripController::FilmstripController(
    timeline::TimelineWidget* timeline,
    thumbnails::ThumbnailManager* manager,
    FilmstripWidget* widget,
    FilmstripControllerConfig config,
    QObject* parent)
    : QObject(parent)
    , timeline_(timeline)
    , manager_(manager)
    , widget_(widget)
    , config_(std::move(config))
{
    setObjectName(QStringLiteral("filmstripController"));
    config_.rangeRefreshDebounceMilliseconds = std::clamp(
        config_.rangeRefreshDebounceMilliseconds,
        0,
        2'000);
    config_.playheadRefreshMilliseconds = std::clamp(
        config_.playheadRefreshMilliseconds,
        0,
        5'000);
    config_.cancelledRetryMilliseconds = std::clamp(
        config_.cancelledRetryMilliseconds,
        25,
        5'000);
    if (!config_.targetSize.isValid()
        || config_.targetSize.width() <= 0
        || config_.targetSize.height() <= 0) {
        config_.targetSize = widget_->preferredThumbnailSize();
    }
    config_.targetSize = config_.targetSize.boundedTo(kMaximumFilmstripThumbnailSize);

    rangeRefreshTimer_.setSingleShot(true);
    rangeRefreshTimer_.setInterval(config_.rangeRefreshDebounceMilliseconds);
    playheadRefreshTimer_.setSingleShot(true);
    playheadRefreshTimer_.setInterval(config_.playheadRefreshMilliseconds);
    cancelledRetryTimer_.setSingleShot(true);
    cancelledRetryTimer_.setInterval(config_.cancelledRetryMilliseconds);

    connect(&rangeRefreshTimer_, &QTimer::timeout, this, &FilmstripController::refreshNow);
    connect(&playheadRefreshTimer_, &QTimer::timeout, this, &FilmstripController::refreshNow);
    connect(
        &cancelledRetryTimer_,
        &QTimer::timeout,
        this,
        &FilmstripController::retryCancelledRequests);
    connect(
        timeline_,
        &timeline::TimelineWidget::viewportChanged,
        this,
        [this](qint64, qint64) {
            if (model_.mode() == filmstrip::FilmstripMode::VisibleTimeline) {
                scheduleRangeRefresh();
            }
        });
    connect(
        timeline_,
        &timeline::TimelineWidget::selectionChanged,
        this,
        [this](qint64, qint64, bool) {
            if (model_.mode() == filmstrip::FilmstripMode::SelectedRange) {
                scheduleRangeRefresh();
            }
        });
    connect(
        manager_,
        &thumbnails::ThumbnailManager::previewReady,
        this,
        &FilmstripController::handlePreview);
    connect(
        manager_,
        &thumbnails::ThumbnailManager::previewFailed,
        this,
        &FilmstripController::handleFailure);
    connect(
        manager_,
        &thumbnails::ThumbnailManager::previewCancelled,
        this,
        &FilmstripController::handleCancellation);
}

FilmstripController::~FilmstripController()
{
    clear();
}

void FilmstripController::setMedia(media::MediaInfoPtr info)
{
    mediaInfo_ = std::move(info);
    playheadNanoseconds_ = static_cast<qint64>(timeline_->model().playhead().count());
    widget_->setPlayhead(playheadNanoseconds_);
    refreshNow();
}

void FilmstripController::clear()
{
    rangeRefreshTimer_.stop();
    playheadRefreshTimer_.stop();
    cancelledRetryTimer_.stop();
    cancelFilmstripRequests();
    pending_.clear();
    cancelledItems_.clear();
    (void)nextBatchGeneration();
    mediaInfo_.reset();
    playheadNanoseconds_ = 0;
    widget_->setPlayhead(0);
    widget_->clear();
}

void FilmstripController::setMode(const filmstrip::FilmstripMode mode)
{
    if (model_.mode() == mode) {
        return;
    }
    model_.setMode(mode);
    refreshNow();
}

filmstrip::FilmstripMode FilmstripController::mode() const noexcept
{
    return model_.mode();
}

void FilmstripController::setCount(const std::size_t count)
{
    const auto previous = model_.count();
    model_.setCount(count);
    if (model_.count() != previous) {
        refreshNow();
    }
}

std::size_t FilmstripController::count() const noexcept
{
    return model_.count();
}

void FilmstripController::setPlayhead(qint64 nanoseconds)
{
    nanoseconds = std::max<qint64>(0, nanoseconds);
    if (playheadNanoseconds_ == nanoseconds) {
        return;
    }
    playheadNanoseconds_ = nanoseconds;
    widget_->setPlayhead(nanoseconds);
    if (model_.mode() == filmstrip::FilmstripMode::AroundCurrentPosition) {
        schedulePlayheadRefresh();
    }
}

void FilmstripController::notifyFrameObserved()
{
    if (model_.mode() == filmstrip::FilmstripMode::AroundCurrentPosition) {
        schedulePlayheadRefresh();
    }
}

void FilmstripController::refreshNow()
{
    rangeRefreshTimer_.stop();
    playheadRefreshTimer_.stop();
    cancelledRetryTimer_.stop();
    cancelFilmstripRequests();
    pending_.clear();
    cancelledItems_.clear();
    const quint64 batch = nextBatchGeneration();

    if (!mediaInfo_) {
        widget_->setPlan(model_.makePlan(timeline_->model()));
        return;
    }

    auto plan = model_.makePlan(timeline_->model());
    widget_->setPlan(plan);
    widget_->setPlayhead(playheadNanoseconds_);
    if (plan.status != filmstrip::FilmstripPlanStatus::Ready || plan.targets.empty()) {
        return;
    }

    std::vector<std::size_t> requestOrder(plan.targets.size());
    std::iota(requestOrder.begin(), requestOrder.end(), std::size_t{0});
    std::stable_sort(
        requestOrder.begin(),
        requestOrder.end(),
        [&](const std::size_t left, const std::size_t right) {
            const auto leftDistance = absoluteDistance(
                static_cast<qint64>(plan.targets[left].requestedTime.count()),
                playheadNanoseconds_);
            const auto rightDistance = absoluteDistance(
                static_cast<qint64>(plan.targets[right].requestedTime.count()),
                playheadNanoseconds_);
            // ThumbnailScheduler dispatches newest work first within a priority.
            // Submit farthest targets first so the playhead-nearest cells run first.
            return leftDistance > rightDistance;
        });

    for (const std::size_t index : requestOrder) {
        submitTarget(index, batch);
    }
}

qsizetype FilmstripController::pendingRequestCount() const noexcept
{
    return pending_.size();
}

quint64 FilmstripController::batchGeneration() const noexcept
{
    return batchGeneration_;
}

void FilmstripController::scheduleRangeRefresh()
{
    if (!mediaInfo_) {
        return;
    }
    rangeRefreshTimer_.start();
}

void FilmstripController::schedulePlayheadRefresh()
{
    if (!mediaInfo_ || playheadRefreshTimer_.isActive()) {
        return;
    }
    playheadRefreshTimer_.start();
}

void FilmstripController::cancelFilmstripRequests() noexcept
{
    manager_->cancelRequests(thumbnails::ThumbnailPriority::VisibleThumbnail);
    manager_->cancelRequests(thumbnails::ThumbnailPriority::NearPlayhead);
}

void FilmstripController::handlePreview(const thumbnails::ThumbnailResult& result)
{
    const auto found = pending_.find(result.request.generation);
    if (found == pending_.end()) {
        return;
    }

    const PendingDelivery delivery = found.value();
    pending_.erase(found);
    if (delivery.batch != batchGeneration_) {
        return;
    }
    (void)widget_->setThumbnail(delivery.itemIndex, result.frame);
}

void FilmstripController::handleFailure(
    const thumbnails::ThumbnailGeneration generation,
    const QString& detail)
{
    const auto found = pending_.find(generation);
    if (found == pending_.end()) {
        return;
    }

    const PendingDelivery delivery = found.value();
    pending_.erase(found);
    if (delivery.batch != batchGeneration_) {
        return;
    }
    (void)widget_->setFailure(delivery.itemIndex, detail);
}

void FilmstripController::handleCancellation(
    const thumbnails::ThumbnailGeneration generation)
{
    const auto found = pending_.find(generation);
    if (found == pending_.end()) {
        return;
    }

    const PendingDelivery delivery = found.value();
    pending_.erase(found);
    if (delivery.batch != batchGeneration_ || !mediaInfo_
        || delivery.itemIndex >= widget_->itemCount()) {
        return;
    }

    const auto* item = widget_->item(delivery.itemIndex);
    if (item == nullptr || item->state != FilmstripItemState::Loading) {
        return;
    }
    if (std::find(cancelledItems_.cbegin(), cancelledItems_.cend(), delivery.itemIndex)
        == cancelledItems_.cend()) {
        cancelledItems_.push_back(delivery.itemIndex);
    }
    cancelledRetryTimer_.start();
}

void FilmstripController::retryCancelledRequests()
{
    if (!mediaInfo_ || cancelledItems_.empty()) {
        cancelledItems_.clear();
        return;
    }

    auto retryItems = std::move(cancelledItems_);
    cancelledItems_.clear();
    const quint64 batch = batchGeneration_;
    const auto& plan = widget_->plan();
    std::stable_sort(
        retryItems.begin(),
        retryItems.end(),
        [&](const std::size_t left, const std::size_t right) {
            if (left >= plan.targets.size() || right >= plan.targets.size()) {
                return left < right;
            }
            const auto leftDistance = absoluteDistance(
                static_cast<qint64>(plan.targets[left].requestedTime.count()),
                playheadNanoseconds_);
            const auto rightDistance = absoluteDistance(
                static_cast<qint64>(plan.targets[right].requestedTime.count()),
                playheadNanoseconds_);
            return leftDistance > rightDistance;
        });
    retryItems.erase(std::unique(retryItems.begin(), retryItems.end()), retryItems.end());

    for (const std::size_t itemIndex : retryItems) {
        if (batch != batchGeneration_) {
            return;
        }
        submitTarget(itemIndex, batch);
    }
}

void FilmstripController::submitTarget(
    const std::size_t itemIndex,
    const quint64 batch)
{
    const auto& plan = widget_->plan();
    if (!mediaInfo_ || batch != batchGeneration_ || itemIndex >= plan.targets.size()) {
        return;
    }

    const auto& target = plan.targets[itemIndex];
    const auto generation = manager_->requestPreview(
        static_cast<qint64>(target.requestedTime.count()),
        config_.targetSize,
        requestPriority(),
        static_cast<qint64>(target.presentationIndexHint));
    if (generation == 0) {
        (void)widget_->setFailure(
            itemIndex,
            tr("The bounded thumbnail queue rejected this request."));
        return;
    }
    pending_.insert(generation, PendingDelivery{batch, itemIndex});
}

thumbnails::ThumbnailPriority FilmstripController::requestPriority() const noexcept
{
    return model_.mode() == filmstrip::FilmstripMode::AroundCurrentPosition
        ? thumbnails::ThumbnailPriority::NearPlayhead
        : thumbnails::ThumbnailPriority::VisibleThumbnail;
}

quint64 FilmstripController::nextBatchGeneration() noexcept
{
    batchGeneration_ = batchGeneration_ == std::numeric_limits<quint64>::max()
        ? quint64{1}
        : batchGeneration_ + quint64{1};
    return batchGeneration_;
}

} // namespace vidscope::widgets
