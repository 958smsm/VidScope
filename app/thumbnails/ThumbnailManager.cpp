#include "thumbnails/ThumbnailManager.h"

#include "core/Logging.h"
#include "media/FrameConverter.h"
#include "playback/PlaybackSession.h"
#include "playback/SeekController.h"
#include "thumbnails/ThumbnailScheduler.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QStandardPaths>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace vidscope::thumbnails {
namespace {

[[nodiscard]] ThumbnailManagerConfig normalizedConfig(ThumbnailManagerConfig config)
{
    config.workerCount = std::clamp<std::size_t>(config.workerCount, 1U, 8U);
    config.maximumPendingRequests = std::max<std::size_t>(1U, config.maximumPendingRequests);
    config.workerQueueFrames = std::max<std::size_t>(1U, config.workerQueueFrames);
    if (!config.maximumThumbnailSize.isValid()
        || config.maximumThumbnailSize.width() <= 0
        || config.maximumThumbnailSize.height() <= 0) {
        config.maximumThumbnailSize = QSize(640, 360);
    }
    if (config.cache.diskDirectory.isEmpty() && config.cache.diskBudgetBytes > 0U) {
        const QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        if (!root.isEmpty()) {
            config.cache.diskDirectory = root + QStringLiteral("/thumbnails");
        }
    }
    return config;
}

[[nodiscard]] playback::PlaybackSessionConfig workerSessionConfig(
    const ThumbnailManagerConfig& config)
{
    playback::PlaybackSessionConfig session;
    session.frameCacheBytes = config.workerFrameCacheBytes;
    session.forwardQueueBytes = config.workerQueueBytes;
    session.forwardQueueFrames = config.workerQueueFrames;
    session.initialPrefetchFrames = 1;
    session.presentationIndexAnchorCount = 4'096;
    session.decoder.hardwareAcceleration = config.hardwareAcceleration;
    session.decoder.allowSoftwareFallback = true;
    return session;
}

[[nodiscard]] QSize boundedTargetSize(QSize requested, QSize maximum) noexcept
{
    if (!requested.isValid() || requested.width() <= 0 || requested.height() <= 0) {
        requested = QSize(320, 180);
    }
    requested = requested.boundedTo(maximum);
    return requested.expandedTo(QSize(1, 1));
}

[[nodiscard]] QSize fittedImageSize(int width, int height, QSize target) noexcept
{
    if (width <= 0 || height <= 0) {
        return {};
    }
    const QSize source(width, height);
    if (source.width() <= target.width() && source.height() <= target.height()) {
        return source;
    }
    return source.scaled(target, Qt::KeepAspectRatio);
}

[[nodiscard]] QString exceptionDetail(const std::exception& exception)
{
    const QString detail = QString::fromUtf8(exception.what());
    return detail.isEmpty() ? QStringLiteral("Thumbnail decoding failed.") : detail;
}

[[nodiscard]] qint64 elapsedMicroseconds(const QElapsedTimer& timer) noexcept
{
    const qint64 nanoseconds = timer.nsecsElapsed();
    return nanoseconds > 0 ? nanoseconds / 1'000 : 0;
}

} // namespace

class ThumbnailManager::Impl final {
public:
    Impl(ThumbnailManager* owner, ThumbnailManagerConfig config)
        : owner_(owner)
        , config_(normalizedConfig(std::move(config)))
        , cache_(config_.cache)
        , scheduler_(config_.maximumPendingRequests, config_.workerCount)
        , sessionConfig_(workerSessionConfig(config_))
    {
        workers_.reserve(config_.workerCount);
        for (std::size_t index = 0; index < config_.workerCount; ++index) {
            workers_.emplace_back([this, index](std::stop_token stop) {
                workerLoop(index, stop);
            });
        }
    }

    ~Impl()
    {
        currentHoverGeneration_.store(0, std::memory_order_release);
        mediaEpoch_.fetch_add(1, std::memory_order_acq_rel);
        scheduler_.close();
        for (auto& worker : workers_) {
            worker.request_stop();
        }
        workers_.clear();
    }

    void setMedia(media::MediaInfoPtr info)
    {
        const std::uint64_t epoch = nextMediaEpoch();
        currentHoverGeneration_.store(0, std::memory_order_release);
        (void)scheduler_.requestMaintenance();

        std::lock_guard lock(mediaMutex_);
        currentInfo_ = std::move(info);
        if (!currentInfo_) {
            currentMedia_.reset();
            return;
        }
        currentMedia_ = ThumbnailMediaSource{
            currentInfo_->path,
            ThumbnailCache::mediaIdentity(*currentInfo_),
            epoch,
        };
    }

    void clearMedia()
    {
        (void)nextMediaEpoch();
        currentHoverGeneration_.store(0, std::memory_order_release);
        (void)scheduler_.requestMaintenance();
        std::lock_guard lock(mediaMutex_);
        currentInfo_.reset();
        currentMedia_.reset();
    }

    [[nodiscard]] ThumbnailGeneration requestPreview(
        qint64 timestampNanoseconds,
        QSize targetSize,
        ThumbnailPriority priority,
        qint64 presentationIndexHint)
    {
        std::optional<ThumbnailMediaSource> mediaSource;
        media::MediaTime duration{};
        {
            std::lock_guard lock(mediaMutex_);
            if (!currentMedia_ || !currentInfo_) {
                return 0;
            }
            mediaSource = currentMedia_;
            duration = currentInfo_->duration;
        }

        const ThumbnailGeneration generation = nextRequestGeneration();
        auto timestamp = media::MediaTime(std::max<qint64>(0, timestampNanoseconds));
        if (duration > media::MediaTime::zero()) {
            timestamp = std::min(timestamp, duration);
        }
        targetSize = boundedTargetSize(targetSize, config_.maximumThumbnailSize);

        ThumbnailRequest request;
        request.generation = generation;
        request.timestamp = timestamp;
        request.targetSize = targetSize;
        request.priority = priority;
        request.presentationIndexHint = presentationIndexHint;

        if (priority == ThumbnailPriority::HoverPreview) {
            currentHoverGeneration_.store(generation, std::memory_order_release);
        }
        scheduler_.supersede(priority);

        ThumbnailCacheKey cacheKey{
            mediaSource->identity,
            static_cast<qint64>(timestamp.count()),
            targetSize,
        };
        if (auto frame = cache_.lookupMemory(cacheKey)) {
            ThumbnailResult result;
            result.request = request;
            result.frame = std::move(*frame);
            result.cacheSource = ThumbnailCacheSource::Memory;
            postResult(std::move(result), mediaSource->epoch);
            return generation;
        }

        ThumbnailJob job;
        job.request = request;
        job.cacheKey = std::move(cacheKey);
        job.media = std::move(*mediaSource);
        job.cancellation = std::make_shared<core::CancellationSource>();
        job.cancellationNotifier = [
            this,
            generation,
            priority,
            mediaEpoch = job.media.epoch
        ] {
            postCancellation(generation, priority, mediaEpoch);
        };
        if (!scheduler_.schedule(std::move(job))) {
            if (priority == ThumbnailPriority::HoverPreview
                && currentHoverGeneration_.load(std::memory_order_acquire) == generation) {
                currentHoverGeneration_.store(0, std::memory_order_release);
            }
            return 0;
        }
        return generation;
    }

    void cancelHoverPreview() noexcept
    {
        cancelRequests(ThumbnailPriority::HoverPreview);
    }

    void cancelRequests(const ThumbnailPriority priority) noexcept
    {
        if (priority == ThumbnailPriority::HoverPreview) {
            currentHoverGeneration_.store(0, std::memory_order_release);
        }
        scheduler_.cancelPriority(priority);
    }

    void cancelAll() noexcept
    {
        currentHoverGeneration_.store(0, std::memory_order_release);
        const std::uint64_t epoch = nextMediaEpoch();
        (void)scheduler_.requestMaintenance();

        // Keep the selected media, but advance its delivery/session epoch so
        // already-queued non-hover results are rejected and future jobs reopen
        // a clean reusable decoder context.
        std::lock_guard lock(mediaMutex_);
        if (currentMedia_) {
            currentMedia_->epoch = epoch;
        }
    }

    [[nodiscard]] ThumbnailCacheStats cacheStats() const
    {
        return cache_.stats();
    }

    [[nodiscard]] bool accepts(
        ThumbnailGeneration generation,
        ThumbnailPriority priority,
        std::uint64_t mediaEpoch) const noexcept
    {
        if (generation == 0 || mediaEpoch_.load(std::memory_order_acquire) != mediaEpoch) {
            return false;
        }
        if (priority == ThumbnailPriority::HoverPreview) {
            return currentHoverGeneration_.load(std::memory_order_acquire) == generation;
        }
        return true;
    }

private:
    struct WorkerCompletion final {
        Impl* owner = nullptr;
        ThumbnailScheduler* scheduler = nullptr;
        std::size_t workerSlot = 0;
        ThumbnailGeneration generation = 0;
        ThumbnailPriority priority = ThumbnailPriority::BackgroundPrecache;
        std::uint64_t mediaEpoch = 0;
        std::shared_ptr<core::CancellationSource> cancellation;

        ~WorkerCompletion()
        {
            if (scheduler != nullptr) {
                scheduler->complete(workerSlot, generation);
            }
            if (owner != nullptr && cancellation
                && cancellation->isCancellationRequested()) {
                owner->postCancellation(generation, priority, mediaEpoch);
            }
        }
    };

    [[nodiscard]] ThumbnailGeneration nextRequestGeneration() noexcept
    {
        auto observed = nextGeneration_.load(std::memory_order_relaxed);
        for (;;) {
            const auto desired = observed == std::numeric_limits<ThumbnailGeneration>::max()
                ? ThumbnailGeneration{1}
                : observed + 1;
            if (nextGeneration_.compare_exchange_weak(
                    observed,
                    desired,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return desired;
            }
        }
    }

    [[nodiscard]] std::uint64_t nextMediaEpoch() noexcept
    {
        auto observed = mediaEpoch_.load(std::memory_order_relaxed);
        for (;;) {
            const auto desired = observed == std::numeric_limits<std::uint64_t>::max()
                ? std::uint64_t{1}
                : observed + 1U;
            if (mediaEpoch_.compare_exchange_weak(
                    observed,
                    desired,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return desired;
            }
        }
    }

    void workerLoop(std::size_t workerSlot, std::stop_token stop)
    {
        playback::PlaybackSession session(sessionConfig_);
        media::FrameConverter converter;
        std::uint64_t loadedMediaEpoch = 0;
        std::uint64_t maintenanceGeneration = 0;
        qCInfo(logThumbnail) << "Thumbnail worker started" << workerSlot;

        while (!stop.stop_requested()) {
            auto taken = scheduler_.waitTake(workerSlot, stop, maintenanceGeneration);
            if (taken.status == ThumbnailTakeStatus::Closed) {
                break;
            }
            if (taken.status == ThumbnailTakeStatus::Maintenance) {
                maintenanceGeneration = taken.maintenanceGeneration;
                loadedMediaEpoch = 0;
                converter.reset();
                session.close();
                continue;
            }
            if (!taken.job) {
                continue;
            }

            ThumbnailJob job = std::move(*taken.job);
            WorkerCompletion completion{
                this,
                &scheduler_,
                workerSlot,
                job.request.generation,
                job.request.priority,
                job.media.epoch,
                job.cancellation,
            };
            const auto cancellation = job.cancellation->token();
            if (cancellation.isCancellationRequested()) {
                continue;
            }

            QElapsedTimer latency;
            latency.start();
            try {
                if (auto cached = cache_.lookupWithSource(job.cacheKey)) {
                    if (cancellation.isCancellationRequested()) {
                        continue;
                    }
                    ThumbnailResult result;
                    result.request = job.request;
                    result.frame = std::move(cached->frame);
                    result.cacheSource = cached->source;
                    result.latencyMicroseconds = elapsedMicroseconds(latency);
                    postResult(std::move(result), job.media.epoch);
                    continue;
                }

                if (loadedMediaEpoch != job.media.epoch || !session.isOpen()) {
                    converter.reset();
                    session.close();
                    const auto opened = session.open(job.media.path, cancellation);
                    if (opened.status == playback::NavigationStatus::Cancelled
                        || cancellation.isCancellationRequested()) {
                        loadedMediaEpoch = 0;
                        continue;
                    }
                    if (!opened) {
                        throw std::runtime_error("The thumbnail worker could not open the video stream");
                    }
                    loadedMediaEpoch = job.media.epoch;
                }

                playback::SeekRequest seek;
                // Thumbnail jobs are intentionally dispatched by priority and
                // newest-interaction order, not monotonically by UI generation.
                // Passing the UI generation into a reusable PlaybackSession
                // would make a later-dispatched older job look stale to that
                // session. The scheduler cancellation token is authoritative
                // here; PlaybackSession still applies its internal operation gate.
                seek.generation = 0;
                seek.target = job.request.timestamp;
                seek.bias = playback::SeekBias::Nearest;
                const auto navigation = session.seek(seek, cancellation);
                if (navigation.status == playback::NavigationStatus::Cancelled
                    || cancellation.isCancellationRequested()) {
                    continue;
                }
                if (!navigation || !navigation.frame) {
                    throw std::runtime_error("No presentation frame was available for the preview target");
                }

                const QSize imageSize = fittedImageSize(
                    navigation.frame->width,
                    navigation.frame->height,
                    job.request.targetSize);
                if (!imageSize.isValid()) {
                    throw std::runtime_error("The preview frame has invalid image dimensions");
                }

                QImage image = converter.toBgraImage(
                    *navigation.frame,
                    imageSize,
                    cancellation);
                if (cancellation.isCancellationRequested()) {
                    continue;
                }
                if (image.isNull()) {
                    throw std::runtime_error("The preview frame conversion was cancelled or failed");
                }

                ThumbnailFrame frame;
                frame.image = std::move(image);
                frame.presentationTime = navigation.frame->presentationTime;
                frame.duration = navigation.frame->duration;
                frame.presentationIndex = navigation.frame->id.presentationIndex;
                frame.pts = navigation.frame->id.pts;
                frame.dts = navigation.frame->dts;
                frame.keyFrame = navigation.frame->keyFrame;
                frame.pictureType = navigation.frame->pictureType;

                // Publish the decoded preview after the bounded memory-cache
                // update. Disk serialization remains off the GUI thread, but
                // it must not extend the user-visible hover latency.
                cache_.insertMemory(job.cacheKey, frame);
                if (cancellation.isCancellationRequested()) {
                    continue;
                }

                ThumbnailResult result;
                result.request = job.request;
                result.frame = frame;
                result.cacheSource = ThumbnailCacheSource::Decoded;
                result.latencyMicroseconds = elapsedMicroseconds(latency);
                postResult(std::move(result), job.media.epoch);

                if (!cancellation.isCancellationRequested()) {
                    cache_.insertDisk(job.cacheKey, frame);
                }
            } catch (const std::exception& exception) {
                if (!cancellation.isCancellationRequested()) {
                    qCWarning(logThumbnail) << "Thumbnail request failed" << exception.what();
                    postFailure(
                        job.request.generation,
                        job.request.priority,
                        job.media.epoch,
                        exceptionDetail(exception));
                }
            } catch (...) {
                if (!cancellation.isCancellationRequested()) {
                    qCWarning(logThumbnail) << "Thumbnail request failed with an unknown exception";
                    postFailure(
                        job.request.generation,
                        job.request.priority,
                        job.media.epoch,
                        QStringLiteral("An unexpected thumbnail decoding error occurred."));
                }
            }
        }

        converter.reset();
        session.close();
        qCInfo(logThumbnail) << "Thumbnail worker stopped" << workerSlot;
    }

    void postResult(ThumbnailResult result, std::uint64_t mediaEpoch)
    {
        QPointer<ThumbnailManager> owner(owner_);
        QMetaObject::invokeMethod(
            owner_,
            [owner, result = std::move(result), mediaEpoch]() mutable {
                if (owner) {
                    owner->deliverResult(std::move(result), mediaEpoch);
                }
            },
            Qt::QueuedConnection);
    }

    void postFailure(
        ThumbnailGeneration generation,
        ThumbnailPriority priority,
        std::uint64_t mediaEpoch,
        QString detail)
    {
        QPointer<ThumbnailManager> owner(owner_);
        QMetaObject::invokeMethod(
            owner_,
            [owner, generation, priority, mediaEpoch, detail = std::move(detail)]() mutable {
                if (owner) {
                    owner->deliverFailure(
                        generation,
                        priority,
                        mediaEpoch,
                        std::move(detail));
                }
            },
            Qt::QueuedConnection);
    }

    void postCancellation(
        const ThumbnailGeneration generation,
        const ThumbnailPriority priority,
        const std::uint64_t mediaEpoch)
    {
        QPointer<ThumbnailManager> owner(owner_);
        QMetaObject::invokeMethod(
            owner_,
            [owner, generation, priority, mediaEpoch] {
                if (owner) {
                    owner->deliverCancellation(generation, priority, mediaEpoch);
                }
            },
            Qt::QueuedConnection);
    }

    ThumbnailManager* const owner_;
    const ThumbnailManagerConfig config_;
    ThumbnailCache cache_;
    ThumbnailScheduler scheduler_;
    const playback::PlaybackSessionConfig sessionConfig_;
    std::vector<std::jthread> workers_;

    mutable std::mutex mediaMutex_;
    media::MediaInfoPtr currentInfo_;
    std::optional<ThumbnailMediaSource> currentMedia_;
    std::atomic<ThumbnailGeneration> nextGeneration_{0};
    std::atomic<ThumbnailGeneration> currentHoverGeneration_{0};
    std::atomic_uint64_t mediaEpoch_{0};
};

ThumbnailManager::ThumbnailManager(ThumbnailManagerConfig config, QObject* parent)
    : QObject(parent)
    , impl_(std::make_unique<Impl>(this, std::move(config)))
{
    qRegisterMetaType<ThumbnailCacheSource>();
    qRegisterMetaType<ThumbnailRequest>();
    qRegisterMetaType<ThumbnailResult>();
}

ThumbnailManager::~ThumbnailManager() = default;

void ThumbnailManager::setMedia(media::MediaInfoPtr info)
{
    impl_->setMedia(std::move(info));
}

void ThumbnailManager::clearMedia()
{
    impl_->clearMedia();
}

ThumbnailGeneration ThumbnailManager::requestPreview(
    qint64 timestampNanoseconds,
    QSize targetSize,
    ThumbnailPriority priority,
    qint64 presentationIndexHint)
{
    return impl_->requestPreview(
        timestampNanoseconds,
        targetSize,
        priority,
        presentationIndexHint);
}

void ThumbnailManager::cancelHoverPreview() noexcept
{
    impl_->cancelHoverPreview();
}

void ThumbnailManager::cancelRequests(const ThumbnailPriority priority) noexcept
{
    impl_->cancelRequests(priority);
}

void ThumbnailManager::cancelAll() noexcept
{
    impl_->cancelAll();
}

ThumbnailCacheStats ThumbnailManager::cacheStats() const
{
    return impl_->cacheStats();
}

void ThumbnailManager::deliverResult(ThumbnailResult result, std::uint64_t mediaEpoch)
{
    if (!impl_->accepts(result.request.generation, result.request.priority, mediaEpoch)) {
        return;
    }

    emit previewReady(result);
    const auto stats = impl_->cacheStats();
    emit cacheStatsChanged(
        static_cast<quint64>(stats.memoryHits),
        static_cast<quint64>(stats.diskHits),
        static_cast<quint64>(stats.misses),
        static_cast<qsizetype>(stats.memoryEntries));
}

void ThumbnailManager::deliverFailure(
    ThumbnailGeneration generation,
    ThumbnailPriority priority,
    std::uint64_t mediaEpoch,
    QString detail)
{
    if (!impl_->accepts(generation, priority, mediaEpoch)) {
        return;
    }
    emit previewFailed(generation, detail);
}

void ThumbnailManager::deliverCancellation(
    const ThumbnailGeneration generation,
    const ThumbnailPriority priority,
    const std::uint64_t mediaEpoch)
{
    if (!impl_->accepts(generation, priority, mediaEpoch)) {
        return;
    }
    emit previewCancelled(generation);
}

} // namespace vidscope::thumbnails
