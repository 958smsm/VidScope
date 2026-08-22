#include "thumbnails/ThumbnailScheduler.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace vidscope::thumbnails {
namespace {

[[nodiscard]] std::uint8_t priorityRank(ThumbnailPriority priority) noexcept
{
    return static_cast<std::uint8_t>(priority);
}

[[nodiscard]] bool isInteractive(ThumbnailPriority priority) noexcept
{
    return priorityRank(priority) <= priorityRank(ThumbnailPriority::UserRequested);
}

} // namespace

class ThumbnailScheduler::Impl final {
public:
    Impl(std::size_t maximumPending, std::size_t workerSlots)
        : maximumPending_(std::max<std::size_t>(1U, maximumPending))
        , active_(std::max<std::size_t>(1U, workerSlots))
    {
    }

    bool schedule(ThumbnailJob job)
    {
        if (!job.cancellation) {
            job.cancellation = std::make_shared<core::CancellationSource>();
        }
        if (job.cancellation->isCancellationRequested()) {
            return false;
        }

        std::lock_guard lock(mutex_);
        if (closed_) {
            job.cancellation->requestCancellation();
            return false;
        }

        job.sequence = nextSequence_ == std::numeric_limits<std::uint64_t>::max()
            ? 1U
            : nextSequence_ + 1U;
        nextSequence_ = job.sequence;

        supersedeLocked(job.request.priority);

        for (auto pending = pending_.begin(); pending != pending_.end();) {
            if (pending->cacheKey != job.cacheKey) {
                ++pending;
                continue;
            }

            const auto existingRank = priorityRank(pending->request.priority);
            const auto incomingRank = priorityRank(job.request.priority);
            if (incomingRank > existingRank) {
                job.cancellation->requestCancellation();
                return false;
            }

            cancelPendingJob(*pending);
            pending = pending_.erase(pending);
        }

        if (pending_.size() >= maximumPending_) {
            const auto worst = std::max_element(
                pending_.begin(),
                pending_.end(),
                [](const ThumbnailJob& left, const ThumbnailJob& right) {
                    const auto leftRank = priorityRank(left.request.priority);
                    const auto rightRank = priorityRank(right.request.priority);
                    if (leftRank != rightRank) {
                        return leftRank < rightRank;
                    }
                    return left.sequence > right.sequence;
                });
            if (worst != pending_.end()) {
                const auto newRank = priorityRank(job.request.priority);
                const auto worstRank = priorityRank(worst->request.priority);
                if (newRank > worstRank) {
                    job.cancellation->requestCancellation();
                    return false;
                }
                cancelPendingJob(*worst);
                pending_.erase(worst);
            }
        }

        pending_.push_back(std::move(job));
        ready_.notify_one();
        return true;
    }

    [[nodiscard]] ThumbnailTakeResult waitTake(
        std::size_t workerSlot,
        std::stop_token stop,
        std::uint64_t observedMaintenanceGeneration)
    {
        std::stop_callback wakeOnStop(stop, [this] { ready_.notify_all(); });
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] {
            return closed_ || stop.stop_requested() || !pending_.empty()
                || maintenanceGeneration_ != observedMaintenanceGeneration;
        });

        if (closed_ || stop.stop_requested()) {
            return {ThumbnailTakeStatus::Closed, std::nullopt, maintenanceGeneration_};
        }
        if (maintenanceGeneration_ != observedMaintenanceGeneration) {
            return {ThumbnailTakeStatus::Maintenance, std::nullopt, maintenanceGeneration_};
        }

        const auto best = std::min_element(
            pending_.begin(),
            pending_.end(),
            [](const ThumbnailJob& left, const ThumbnailJob& right) {
                const auto leftRank = priorityRank(left.request.priority);
                const auto rightRank = priorityRank(right.request.priority);
                if (leftRank != rightRank) {
                    return leftRank < rightRank;
                }
                return left.sequence > right.sequence;
            });
        if (best == pending_.end()) {
            return {ThumbnailTakeStatus::Maintenance, std::nullopt, maintenanceGeneration_};
        }

        ThumbnailJob job = std::move(*best);
        pending_.erase(best);
        if (workerSlot < active_.size()) {
            active_[workerSlot] = ActiveJob{
                job.request.generation,
                job.request.priority,
                job.cancellation,
            };
        }
        return {
            ThumbnailTakeStatus::Job,
            std::optional<ThumbnailJob>{std::move(job)},
            maintenanceGeneration_,
        };
    }

    void complete(std::size_t workerSlot, ThumbnailGeneration generation) noexcept
    {
        std::lock_guard lock(mutex_);
        if (workerSlot >= active_.size() || !active_[workerSlot]
            || active_[workerSlot]->generation != generation) {
            return;
        }
        active_[workerSlot].reset();
    }

    [[nodiscard]] std::uint64_t requestMaintenance()
    {
        std::lock_guard lock(mutex_);
        cancelAllLocked();
        maintenanceGeneration_ = maintenanceGeneration_ == std::numeric_limits<std::uint64_t>::max()
            ? 1U
            : maintenanceGeneration_ + 1U;
        ready_.notify_all();
        return maintenanceGeneration_;
    }

    void supersede(ThumbnailPriority priority) noexcept
    {
        std::lock_guard lock(mutex_);
        supersedeLocked(priority);
        ready_.notify_all();
    }

    void cancelPriority(ThumbnailPriority priority) noexcept
    {
        std::lock_guard lock(mutex_);
        for (auto pending = pending_.begin(); pending != pending_.end();) {
            if (pending->request.priority != priority) {
                ++pending;
                continue;
            }
            cancelPendingJob(*pending);
            pending = pending_.erase(pending);
        }
        for (auto& active : active_) {
            if (active && active->priority == priority && active->cancellation) {
                active->cancellation->requestCancellation();
            }
        }
        ready_.notify_all();
    }

    void cancelAll() noexcept
    {
        std::lock_guard lock(mutex_);
        cancelAllLocked();
        ready_.notify_all();
    }

    void close() noexcept
    {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return;
        }
        closed_ = true;
        cancelAllLocked();
        ready_.notify_all();
    }

    [[nodiscard]] std::size_t pendingCount() const noexcept
    {
        std::lock_guard lock(mutex_);
        return pending_.size();
    }

    [[nodiscard]] bool isClosed() const noexcept
    {
        std::lock_guard lock(mutex_);
        return closed_;
    }

private:
    struct ActiveJob final {
        ThumbnailGeneration generation = 0;
        ThumbnailPriority priority = ThumbnailPriority::BackgroundPrecache;
        std::shared_ptr<core::CancellationSource> cancellation;
    };

    static void cancelPendingJob(ThumbnailJob& job) noexcept
    {
        if (job.cancellation) {
            job.cancellation->requestCancellation();
        }
        if (!job.cancellationNotifier) {
            return;
        }

        // Notification is deliberately best-effort and noexcept because the
        // scheduler's cancellation and shutdown paths must never unwind.
        try {
            job.cancellationNotifier();
        } catch (...) {
        }
        job.cancellationNotifier = {};
    }

    void supersedeLocked(ThumbnailPriority priority) noexcept
    {
        if (priority == ThumbnailPriority::HoverPreview) {
            for (auto pending = pending_.begin(); pending != pending_.end();) {
                if (pending->request.priority != ThumbnailPriority::HoverPreview) {
                    ++pending;
                    continue;
                }
                cancelPendingJob(*pending);
                pending = pending_.erase(pending);
            }
        }

        if (!isInteractive(priority)) {
            return;
        }
        for (auto& active : active_) {
            if (!active || !active->cancellation) {
                continue;
            }
            if (priorityRank(active->priority) >= priorityRank(priority)) {
                active->cancellation->requestCancellation();
            }
        }
    }

    void cancelAllLocked() noexcept
    {
        for (auto& job : pending_) {
            cancelPendingJob(job);
        }
        pending_.clear();
        for (auto& active : active_) {
            if (active && active->cancellation) {
                active->cancellation->requestCancellation();
            }
        }
    }

    const std::size_t maximumPending_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<ThumbnailJob> pending_;
    std::vector<std::optional<ActiveJob>> active_;
    std::uint64_t nextSequence_ = 0;
    std::uint64_t maintenanceGeneration_ = 0;
    bool closed_ = false;
};

ThumbnailScheduler::ThumbnailScheduler(std::size_t maximumPending, std::size_t workerSlots)
    : impl_(std::make_unique<Impl>(maximumPending, workerSlots))
{
}

ThumbnailScheduler::~ThumbnailScheduler()
{
    close();
}

bool ThumbnailScheduler::schedule(ThumbnailJob job)
{
    return impl_->schedule(std::move(job));
}

ThumbnailTakeResult ThumbnailScheduler::waitTake(
    std::size_t workerSlot,
    std::stop_token stop,
    std::uint64_t observedMaintenanceGeneration)
{
    return impl_->waitTake(workerSlot, stop, observedMaintenanceGeneration);
}

void ThumbnailScheduler::complete(
    std::size_t workerSlot,
    ThumbnailGeneration generation) noexcept
{
    impl_->complete(workerSlot, generation);
}

std::uint64_t ThumbnailScheduler::requestMaintenance()
{
    return impl_->requestMaintenance();
}

void ThumbnailScheduler::supersede(ThumbnailPriority priority) noexcept
{
    impl_->supersede(priority);
}

void ThumbnailScheduler::cancelPriority(ThumbnailPriority priority) noexcept
{
    impl_->cancelPriority(priority);
}

void ThumbnailScheduler::cancelAll() noexcept
{
    impl_->cancelAll();
}

void ThumbnailScheduler::close() noexcept
{
    impl_->close();
}

std::size_t ThumbnailScheduler::pendingCount() const noexcept
{
    return impl_->pendingCount();
}

bool ThumbnailScheduler::isClosed() const noexcept
{
    return impl_->isClosed();
}

} // namespace vidscope::thumbnails
