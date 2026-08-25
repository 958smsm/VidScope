#include "analysis/AnalysisManager.h"

#include "analysis/VideoAnalyzer.h"
#include "core/Logging.h"
#include "playback/SeekController.h"

#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QStandardPaths>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

namespace vidscope::analysis {
namespace {

enum class TaskKind : std::uint8_t {
    Initialize,
    Range,
};

struct AnalysisTask final {
    TaskKind kind = TaskKind::Range;
    AnalysisPriority priority = AnalysisPriority::Background;
    media::MediaTime start{};
    media::MediaTime end{};
    std::uint64_t epoch = 0;
    std::uint64_t sequence = 0;
};

[[nodiscard]] bool taskPrecedes(const AnalysisTask& left, const AnalysisTask& right) noexcept
{
    if (left.kind != right.kind) {
        return left.kind == TaskKind::Initialize;
    }
    if (left.priority != right.priority) {
        return left.priority < right.priority;
    }
    return left.sequence > right.sequence;
}

[[nodiscard]] media::MediaTime clampedTime(
    const media::MediaInfo& info,
    media::MediaTime time) noexcept
{
    time = std::max(time, media::MediaTime::zero());
    if (info.duration > media::MediaTime::zero()) {
        time = std::min(time, info.duration);
    }
    return time;
}

[[nodiscard]] double estimatedProgress(
    const media::MediaInfo& info,
    const AnalysisStore& store) noexcept
{
    const std::size_t sampleCount = store.size();
    if (sampleCount == 0) {
        return 0.0;
    }
    if (info.declaredFrameCount > 0) {
        return std::clamp(
            static_cast<double>(sampleCount) / static_cast<double>(info.declaredFrameCount),
            0.0,
            1.0);
    }
    if (info.duration <= media::MediaTime::zero()) {
        return 0.0;
    }
    const auto covered = store.latestPresentationEnd();
    if (!covered) {
        return 0.0;
    }
    return std::clamp(
        static_cast<double>(covered->count()) / static_cast<double>(info.duration.count()),
        0.0,
        1.0);
}

[[nodiscard]] std::size_t estimatedAnalysisSamples(
    const media::MediaInfo& info,
    const std::size_t maximumSamples) noexcept
{
    std::uint64_t estimate = 1;
    if (info.declaredFrameCount > 0) {
        estimate = static_cast<std::uint64_t>(info.declaredFrameCount);
    } else {
        media::MediaTime frameDuration = media::nominalFrameDuration(info.averageFrameRate);
        if (frameDuration <= media::MediaTime::zero()) {
            frameDuration = media::nominalFrameDuration(info.realFrameRate);
        }
        if (frameDuration <= media::MediaTime::zero()) {
            frameDuration = std::chrono::nanoseconds(33'333'333);
        }
        if (info.duration > media::MediaTime::zero()) {
            estimate = static_cast<std::uint64_t>(info.duration.count() / frameDuration.count()) + 1U;
        }
    }
    return static_cast<std::size_t>(std::min<std::uint64_t>(
        estimate,
        static_cast<std::uint64_t>(maximumSamples)));
}

} // namespace

class AnalysisManager::Impl final {
public:
    Impl(AnalysisManager* owner, AnalysisManagerConfig config)
        : owner_(owner)
        , config_(normalizeConfig(std::move(config)))
        , store_(config_.maximumInMemorySamples)
        , pyramid_(config_.pyramid)
        , cache_(config_.cache)
        , worker_([this](const std::stop_token stop) { run(stop); })
    {
    }

    ~Impl()
    {
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
            activeCancellation_.requestCancellation();
            tasks_.clear();
        }
        condition_.notify_all();
        worker_.request_stop();
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void setMedia(media::MediaInfoPtr info)
    {
        if (!info) {
            clearMedia();
            return;
        }
        const auto duration = info->duration;
        const auto estimatedSamples = estimatedAnalysisSamples(
            *info,
            config_.maximumInMemorySamples);
        std::uint64_t epoch = 0;
        {
            std::lock_guard lock(mutex_);
            epoch = nextEpochLocked();
            media_ = std::move(info);
            activeCancellation_.requestCancellation();
            tasks_.clear();
            tasks_.push_back({
                TaskKind::Initialize,
                AnalysisPriority::AroundPlayhead,
                {},
                {},
                epoch,
                nextSequenceLocked(),
            });
            state_.store(AnalysisState::LoadingCache, std::memory_order_release);
            progress_.store(0.0, std::memory_order_release);
        }
        store_.clear();
        pyramid_.reset(duration, estimatedSamples);
        condition_.notify_all();
        postState(AnalysisState::LoadingCache, epoch);
        postProgress(0.0, 0, epoch);
    }

    void clearMedia()
    {
        {
            std::lock_guard lock(mutex_);
            (void)nextEpochLocked();
            media_.reset();
            activeCancellation_.requestCancellation();
            tasks_.clear();
            state_.store(AnalysisState::Idle, std::memory_order_release);
            progress_.store(0.0, std::memory_order_release);
        }
        store_.clear();
        pyramid_.clear();
        condition_.notify_all();
    }

    void setPlaybackActive(const bool active)
    {
        std::uint64_t epoch = 0;
        bool changed = false;
        {
            std::lock_guard lock(mutex_);
            changed = playbackActive_ != active;
            playbackActive_ = active;
            epoch = epoch_;
            if (active && activeTask_ && activeTask_->kind == TaskKind::Range) {
                activeCancellation_.requestCancellation();
            }
        }
        condition_.notify_all();
        if (changed && epoch != 0) {
            const bool paused = suspended();
            state_.store(
                paused ? AnalysisState::Paused : AnalysisState::Analyzing,
                std::memory_order_release);
            postState(paused ? AnalysisState::Paused : AnalysisState::Analyzing, epoch);
        }
    }

    void setInteractiveActivity(const bool active)
    {
        std::uint64_t epoch = 0;
        bool changed = false;
        {
            std::lock_guard lock(mutex_);
            changed = interactiveActive_ != active;
            interactiveActive_ = active;
            epoch = epoch_;
            if (active && activeTask_ && activeTask_->kind == TaskKind::Range) {
                activeCancellation_.requestCancellation();
            }
        }
        condition_.notify_all();
        if (changed && epoch != 0) {
            const bool paused = suspended();
            state_.store(
                paused ? AnalysisState::Paused : AnalysisState::Analyzing,
                std::memory_order_release);
            postState(paused ? AnalysisState::Paused : AnalysisState::Analyzing, epoch);
        }
    }

    void requestPlayhead(const qint64 nanoseconds)
    {
        queueRange(
            media::MediaTime(std::max<qint64>(0, nanoseconds)) - config_.playheadRadius,
            media::MediaTime(std::max<qint64>(0, nanoseconds)) + config_.playheadRadius,
            AnalysisPriority::AroundPlayhead);
    }

    void requestVisibleRange(const qint64 start, const qint64 end)
    {
        queueRange(
            media::MediaTime(std::max<qint64>(0, start)),
            media::MediaTime(std::max<qint64>(0, end)),
            AnalysisPriority::VisibleRange);
    }

    [[nodiscard]] std::optional<AnalysisSample> sampleFor(
        const qint64 timestamp,
        const qint64 presentationIndex) const
    {
        return store_.nearest(
            media::MediaTime(std::max<qint64>(0, timestamp)),
            presentationIndex);
    }

    [[nodiscard]] const AnalysisStore& store() const noexcept { return store_; }
    [[nodiscard]] AnalysisLodView lodView(
        const qint64 start,
        const qint64 end,
        const std::size_t maximumBuckets) const
    {
        return pyramid_.view(
            media::MediaTime(std::max<qint64>(0, start)),
            media::MediaTime(std::max<qint64>(0, end)),
            maximumBuckets);
    }
    [[nodiscard]] double progress() const noexcept { return progress_.load(std::memory_order_acquire); }
    [[nodiscard]] AnalysisState state() const noexcept { return state_.load(std::memory_order_acquire); }

    [[nodiscard]] bool acceptsEpoch(const std::uint64_t epoch) const noexcept
    {
        std::lock_guard lock(mutex_);
        return epoch != 0 && epoch == epoch_ && static_cast<bool>(media_);
    }

private:
    struct WorkerContext final {
        std::uint64_t epoch = 0;
        media::MediaInfoPtr info;
        std::unique_ptr<playback::PlaybackSession> session;
        std::unique_ptr<LumaExtractor> extractor;
    };

    struct RangeOutcome final {
        bool cancelled = false;
        bool reachedEnd = false;
        bool capacityExceeded = false;
        bool failed = false;
        media::MediaTime resumeTime{};
    };

    [[nodiscard]] static AnalysisManagerConfig normalizeConfig(AnalysisManagerConfig config)
    {
        config.maximumInMemorySamples = std::max<std::size_t>(1, config.maximumInMemorySamples);
        config.cache.maximumSamples = std::min(
            config.cache.maximumSamples,
            config.maximumInMemorySamples);
        config.deliveryBatchFrames = std::max<std::size_t>(1, config.deliveryBatchFrames);
        if (!config.lumaSize.isValid() || config.lumaSize.width() <= 0
            || config.lumaSize.height() <= 0 || config.lumaSize.width() > 2'048
            || config.lumaSize.height() > 2'048) {
            config.lumaSize = QSize(160, 90);
        }
        config.playheadRadius = std::max(config.playheadRadius, media::MediaTime::zero());
        config.rangePreroll = std::max(config.rangePreroll, media::MediaTime::zero());
        if (config.cache.diskDirectory.isEmpty()) {
            config.cache.diskDirectory = QStandardPaths::writableLocation(
                QStandardPaths::CacheLocation) + QStringLiteral("/analysis");
        }
        config.session.frameCacheBytes = std::min<std::size_t>(
            config.session.frameCacheBytes,
            32ULL * 1024ULL * 1024ULL);
        config.session.forwardQueueBytes = std::min<std::size_t>(
            config.session.forwardQueueBytes,
            16ULL * 1024ULL * 1024ULL);
        config.session.forwardQueueFrames = std::clamp<std::size_t>(
            config.session.forwardQueueFrames,
            1,
            4);
        config.session.initialPrefetchFrames = std::min<std::size_t>(
            config.session.initialPrefetchFrames,
            2);
        config.session.decoder.hardwareAcceleration = media::HardwareAcceleration::Disabled;
        return config;
    }

    void queueRange(
        media::MediaTime start,
        media::MediaTime end,
        const AnalysisPriority priority)
    {
        std::lock_guard lock(mutex_);
        if (!media_) {
            return;
        }
        if (state_.load(std::memory_order_acquire) == AnalysisState::Complete) {
            return;
        }
        start = clampedTime(*media_, start);
        end = clampedTime(*media_, end);
        if (end < start) {
            std::swap(start, end);
        }

        tasks_.erase(
            std::remove_if(tasks_.begin(), tasks_.end(), [&](const AnalysisTask& task) {
                return task.kind == TaskKind::Range && task.priority == priority;
            }),
            tasks_.end());
        tasks_.push_back({
            TaskKind::Range,
            priority,
            start,
            end,
            epoch_,
            nextSequenceLocked(),
        });

        if (activeTask_ && activeTask_->kind == TaskKind::Range
            && (priority <= activeTask_->priority
                || activeTask_->priority == AnalysisPriority::Background)) {
            activeCancellation_.requestCancellation();
        }
        condition_.notify_all();
    }

    [[nodiscard]] std::uint64_t nextEpochLocked() noexcept
    {
        epoch_ = epoch_ == std::numeric_limits<std::uint64_t>::max() ? 1 : epoch_ + 1;
        return epoch_;
    }

    [[nodiscard]] std::uint64_t nextSequenceLocked() noexcept
    {
        sequence_ = sequence_ == std::numeric_limits<std::uint64_t>::max() ? 1 : sequence_ + 1;
        return sequence_;
    }

    [[nodiscard]] std::optional<AnalysisTask> takeTask(const std::stop_token stop)
    {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, stop, [&] {
            if (closing_) {
                return true;
            }
            return std::any_of(tasks_.begin(), tasks_.end(), [&](const AnalysisTask& task) {
                return task.kind == TaskKind::Initialize || !suspendedLocked();
            });
        });
        if (closing_ || stop.stop_requested()) {
            return std::nullopt;
        }

        auto best = tasks_.end();
        for (auto candidate = tasks_.begin(); candidate != tasks_.end(); ++candidate) {
            if (candidate->kind != TaskKind::Initialize && suspendedLocked()) {
                continue;
            }
            if (best == tasks_.end() || taskPrecedes(*candidate, *best)) {
                best = candidate;
            }
        }
        if (best == tasks_.end()) {
            return std::nullopt;
        }
        AnalysisTask task = *best;
        tasks_.erase(best);
        activeTask_ = task;
        activeCancellation_ = core::CancellationSource{};
        return task;
    }

    void finishTask()
    {
        std::lock_guard lock(mutex_);
        activeTask_.reset();
    }

    [[nodiscard]] core::CancellationToken activeToken() const
    {
        std::lock_guard lock(mutex_);
        return activeCancellation_.token();
    }

    void run(const std::stop_token stop)
    {
        WorkerContext context;
        while (!stop.stop_requested()) {
            const auto task = takeTask(stop);
            if (!task) {
                if (stop.stop_requested()) {
                    return;
                }
                continue;
            }

            if (task->kind == TaskKind::Initialize) {
                try {
                    initialize(*task, context);
                } catch (const std::exception& error) {
                    state_.store(AnalysisState::Error, std::memory_order_release);
                    postError(QString::fromUtf8(error.what()), task->epoch);
                    postState(AnalysisState::Error, task->epoch);
                }
                finishTask();
                continue;
            }
            if (task->epoch != context.epoch || !context.info) {
                finishTask();
                continue;
            }

            postState(AnalysisState::Analyzing, task->epoch);
            const RangeOutcome outcome = analyzeRange(*task, context, activeToken());
            finishTask();
            if (!acceptsEpoch(task->epoch)) {
                continue;
            }
            if (outcome.capacityExceeded) {
                state_.store(AnalysisState::Error, std::memory_order_release);
                postError(
                    QStringLiteral("Analysis reached the configured %1-sample memory bound.")
                        .arg(static_cast<qulonglong>(store_.capacity())),
                    task->epoch);
                postState(AnalysisState::Error, task->epoch);
                continue;
            }
            if (outcome.failed) {
                state_.store(AnalysisState::Error, std::memory_order_release);
                postState(AnalysisState::Error, task->epoch);
                continue;
            }

            const bool complete = task->priority == AnalysisPriority::Background
                && !outcome.cancelled && outcome.reachedEnd;
            (void)cache_.save(*context.info, store_.snapshot(), complete);
            if (complete) {
                {
                    std::lock_guard lock(mutex_);
                    tasks_.erase(
                        std::remove_if(tasks_.begin(), tasks_.end(), [&](const AnalysisTask& pending) {
                            return pending.epoch == task->epoch && pending.kind == TaskKind::Range;
                        }),
                        tasks_.end());
                }
                progress_.store(1.0, std::memory_order_release);
                state_.store(AnalysisState::Complete, std::memory_order_release);
                postProgress(1.0, store_.size(), task->epoch);
                postState(AnalysisState::Complete, task->epoch);
            } else if (outcome.cancelled && task->priority == AnalysisPriority::Background) {
                requeueBackground(*task, outcome.resumeTime);
            }
        }
    }

    void initialize(const AnalysisTask& task, WorkerContext& context)
    {
        media::MediaInfoPtr info;
        {
            std::lock_guard lock(mutex_);
            if (task.epoch != epoch_ || !media_) {
                return;
            }
            info = media_;
        }

        postState(AnalysisState::LoadingCache, task.epoch);
        AnalysisCacheDocument document = cache_.load(*info);
        if (!acceptsEpoch(task.epoch)) {
            return;
        }
        store_.replace(std::move(document.samples));
        pyramid_.rebuild(store_.snapshot());
        context.epoch = task.epoch;
        context.info = info;
        context.session = std::make_unique<playback::PlaybackSession>(config_.session);
        context.extractor = std::make_unique<LumaExtractor>(config_.lumaSize);

        const double restoredProgress = document.complete
            ? 1.0
            : estimatedProgress(*info, store_);
        progress_.store(restoredProgress, std::memory_order_release);
        if (store_.size() > 0) {
            postSamples(0, static_cast<qint64>(info->duration.count()), store_.size(), task.epoch);
        }
        postProgress(restoredProgress, store_.size(), task.epoch);

        if (document.complete) {
            {
                std::lock_guard lock(mutex_);
                tasks_.erase(
                    std::remove_if(tasks_.begin(), tasks_.end(), [&](const AnalysisTask& pending) {
                        return pending.epoch == task.epoch && pending.kind == TaskKind::Range;
                    }),
                    tasks_.end());
            }
            state_.store(AnalysisState::Complete, std::memory_order_release);
            postState(AnalysisState::Complete, task.epoch);
            return;
        }

        {
            std::lock_guard lock(mutex_);
            if (task.epoch != epoch_ || !media_) {
                return;
            }
            const bool alreadyQueued = std::any_of(
                tasks_.begin(), tasks_.end(), [](const AnalysisTask& pending) {
                    return pending.kind == TaskKind::Range
                        && pending.priority == AnalysisPriority::Background;
                });
            if (!alreadyQueued) {
                tasks_.push_back({
                    TaskKind::Range,
                    AnalysisPriority::Background,
                    media::MediaTime::zero(),
                    info->duration,
                    task.epoch,
                    nextSequenceLocked(),
                });
            }
        }
        condition_.notify_all();
    }

    [[nodiscard]] RangeOutcome analyzeRange(
        const AnalysisTask& task,
        WorkerContext& context,
        const core::CancellationToken cancellation)
    {
        RangeOutcome outcome;
        outcome.resumeTime = task.start;
        try {
            if (!context.session->isOpen()) {
                const auto opened = context.session->open(context.info->path, cancellation);
                if (opened.status == playback::NavigationStatus::Cancelled) {
                    outcome.cancelled = true;
                    return outcome;
                }
                if (!opened) {
                    postError(QStringLiteral("The analysis decoder could not open the video."), task.epoch);
                    outcome.failed = true;
                    return outcome;
                }
            }

            const media::MediaTime decodeStart = task.start > config_.rangePreroll
                ? task.start - config_.rangePreroll
                : media::MediaTime::zero();
            const auto requestGeneration = task.sequence == 0 ? std::uint64_t{1} : task.sequence;
            auto navigation = context.session->seek(
                playback::SeekRequest{
                    requestGeneration,
                    decodeStart,
                    playback::SeekBias::AtOrAfter,
                },
                cancellation);
            if (navigation.status == playback::NavigationStatus::Cancelled) {
                outcome.cancelled = true;
                return outcome;
            }

            std::optional<LumaPlane> previous;
            std::size_t batchCount = 0;
            media::MediaTime batchStart{};
            media::MediaTime batchEnd{};
            bool batchStarted = false;

            auto flushBatch = [&] {
                if (!batchStarted || batchCount == 0) {
                    return;
                }
                if (!acceptsEpoch(task.epoch)) {
                    batchCount = 0;
                    batchStarted = false;
                    return;
                }
                const auto aligned = pyramid_.alignedBaseRange(batchStart, batchEnd);
                const auto samples = store_.range(
                    aligned.start,
                    aligned.end,
                    store_.capacity());
                pyramid_.replaceRange(batchStart, batchEnd, samples);
                postSamples(
                    static_cast<qint64>(batchStart.count()),
                    static_cast<qint64>(batchEnd.count()),
                    store_.size(),
                    task.epoch);
                const double value = estimatedProgress(*context.info, store_);
                progress_.store(value, std::memory_order_release);
                postProgress(value, store_.size(), task.epoch);
                batchCount = 0;
                batchStarted = false;
            };

            while (navigation) {
                if (cancellation.isCancellationRequested()) {
                    outcome.cancelled = true;
                    break;
                }
                const auto& frame = navigation.frame;
                if (!frame || frame->presentationTime == media::kNoMediaTime) {
                    navigation = context.session->nextFrame(cancellation);
                    continue;
                }
                if (frame->presentationTime > task.end) {
                    outcome.reachedEnd = true;
                    break;
                }

                LumaPlane current = context.extractor->extract(*frame, cancellation);
                if (!current.isValid()) {
                    outcome.cancelled = cancellation.isCancellationRequested();
                    break;
                }
                if (cancellation.isCancellationRequested()) {
                    outcome.cancelled = true;
                    break;
                }

                AnalysisSample sample;
                sample.presentationTime = frame->presentationTime;
                sample.duration = std::max(frame->duration, media::MediaTime::zero());
                sample.presentationIndex = frame->id.presentationIndex;
                sample.pts = frame->id.pts;
                sample.keyFrame = frame->keyFrame;
                if (previous) {
                    sample.motion = VideoAnalyzer::motionScore(*previous, current);
                    sample.similarity = VideoAnalyzer::similarityScore(*previous, current);
                }

                if (frame->presentationTime >= task.start) {
                    if (!store_.upsert(sample)) {
                        outcome.capacityExceeded = true;
                        break;
                    }
                    outcome.resumeTime = frame->presentationTime
                        + std::max(frame->duration, std::chrono::nanoseconds(1));
                    if (!batchStarted) {
                        batchStart = frame->presentationTime;
                        batchStarted = true;
                    }
                    batchEnd = frame->presentationTime;
                    ++batchCount;
                    if (batchCount >= config_.deliveryBatchFrames) {
                        flushBatch();
                    }
                }
                previous = std::move(current);
                navigation = context.session->nextFrame(cancellation);
            }

            if (navigation.status == playback::NavigationStatus::EndOfStream) {
                outcome.reachedEnd = true;
            } else if (navigation.status == playback::NavigationStatus::Cancelled) {
                outcome.cancelled = true;
            }
            flushBatch();
        } catch (const std::exception& error) {
            postError(QString::fromUtf8(error.what()), task.epoch);
            outcome.failed = true;
        }
        return outcome;
    }

    void requeueBackground(const AnalysisTask& original, media::MediaTime resume)
    {
        std::lock_guard lock(mutex_);
        if (original.epoch != epoch_ || !media_ || resume > original.end) {
            return;
        }
        const bool queued = std::any_of(tasks_.begin(), tasks_.end(), [](const AnalysisTask& task) {
            return task.kind == TaskKind::Range
                && task.priority == AnalysisPriority::Background;
        });
        if (!queued) {
            tasks_.push_back({
                TaskKind::Range,
                AnalysisPriority::Background,
                resume,
                original.end,
                original.epoch,
                nextSequenceLocked(),
            });
        }
        condition_.notify_all();
    }

    void postSamples(
        const qint64 start,
        const qint64 end,
        const std::size_t count,
        const std::uint64_t epoch) const
    {
        QPointer<AnalysisManager> guard(owner_);
        QMetaObject::invokeMethod(
            owner_,
            [guard, start, end, count, epoch] {
                if (guard) {
                    guard->deliverSamples(
                        start,
                        end,
                        static_cast<quint64>(count),
                        static_cast<quint64>(epoch));
                }
            },
            Qt::QueuedConnection);
    }

    void postProgress(
        const double value,
        const std::size_t count,
        const std::uint64_t epoch) const
    {
        QPointer<AnalysisManager> guard(owner_);
        QMetaObject::invokeMethod(
            owner_,
            [guard, value, count, epoch] {
                if (guard) {
                    guard->deliverProgress(
                        value,
                        static_cast<quint64>(count),
                        static_cast<quint64>(epoch));
                }
            },
            Qt::QueuedConnection);
    }

    void postState(const AnalysisState state, const std::uint64_t epoch) const
    {
        QPointer<AnalysisManager> guard(owner_);
        QMetaObject::invokeMethod(
            owner_,
            [guard, state, epoch] {
                if (guard) {
                    guard->deliverState(state, static_cast<quint64>(epoch));
                }
            },
            Qt::QueuedConnection);
    }

    void postError(QString detail, const std::uint64_t epoch) const
    {
        QPointer<AnalysisManager> guard(owner_);
        QMetaObject::invokeMethod(
            owner_,
            [guard, detail = std::move(detail), epoch]() mutable {
                if (guard) {
                    guard->deliverError(std::move(detail), static_cast<quint64>(epoch));
                }
            },
            Qt::QueuedConnection);
    }

    AnalysisManager* const owner_;
    AnalysisManagerConfig config_;
    AnalysisStore store_;
    AnalysisPyramid pyramid_;
    AnalysisCache cache_;

    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    media::MediaInfoPtr media_;
    std::deque<AnalysisTask> tasks_;
    std::optional<AnalysisTask> activeTask_;
    core::CancellationSource activeCancellation_;
    std::uint64_t epoch_ = 0;
    std::uint64_t sequence_ = 0;
    bool playbackActive_ = false;
    bool interactiveActive_ = false;
    bool closing_ = false;

    std::atomic<double> progress_{0.0};
    std::atomic<AnalysisState> state_{AnalysisState::Idle};
    std::jthread worker_;

    [[nodiscard]] bool suspendedLocked() const noexcept
    {
        return playbackActive_ || interactiveActive_;
    }

    [[nodiscard]] bool suspended() const noexcept
    {
        std::lock_guard lock(mutex_);
        return suspendedLocked();
    }
};

AnalysisManager::AnalysisManager(AnalysisManagerConfig config, QObject* parent)
    : QObject(parent)
    , impl_(std::make_unique<Impl>(this, std::move(config)))
{
    setObjectName(QStringLiteral("analysisManager"));
}

AnalysisManager::~AnalysisManager() = default;

void AnalysisManager::setMedia(media::MediaInfoPtr info)
{
    impl_->setMedia(std::move(info));
}

void AnalysisManager::clearMedia()
{
    impl_->clearMedia();
    emit stateChanged(AnalysisState::Idle);
    emit progressChanged(0.0, 0);
}

void AnalysisManager::setPlaybackActive(const bool active)
{
    impl_->setPlaybackActive(active);
}

void AnalysisManager::setInteractiveActivity(const bool active)
{
    impl_->setInteractiveActivity(active);
}

void AnalysisManager::requestPlayhead(const qint64 timestampNanoseconds)
{
    impl_->requestPlayhead(timestampNanoseconds);
}

void AnalysisManager::requestVisibleRange(
    const qint64 startNanoseconds,
    const qint64 endNanoseconds)
{
    impl_->requestVisibleRange(startNanoseconds, endNanoseconds);
}

std::optional<AnalysisSample> AnalysisManager::sampleFor(
    const qint64 timestampNanoseconds,
    const qint64 presentationIndex) const
{
    return impl_->sampleFor(timestampNanoseconds, presentationIndex);
}

std::vector<AnalysisSample> AnalysisManager::samplesInRange(
    const qint64 startNanoseconds,
    const qint64 endNanoseconds,
    const std::size_t maximumResults) const
{
    auto start = media::MediaTime(std::max<qint64>(0, startNanoseconds));
    auto end = media::MediaTime(std::max<qint64>(0, endNanoseconds));
    return impl_->store().range(start, end, maximumResults);
}

AnalysisLodView AnalysisManager::lodView(
    const qint64 startNanoseconds,
    const qint64 endNanoseconds,
    const std::size_t maximumBuckets) const
{
    return impl_->lodView(startNanoseconds, endNanoseconds, maximumBuckets);
}

qsizetype AnalysisManager::sampleCount() const noexcept
{
    const auto count = impl_->store().size();
    return static_cast<qsizetype>(std::min<std::size_t>(
        count,
        static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())));
}

double AnalysisManager::progress() const noexcept
{
    return impl_->progress();
}

AnalysisState AnalysisManager::state() const noexcept
{
    return impl_->state();
}

void AnalysisManager::deliverSamples(
    const qint64 start,
    const qint64 end,
    const quint64 count,
    const quint64 epoch)
{
    if (impl_->acceptsEpoch(epoch)) {
        emit samplesAvailable(start, end, count);
    }
}

void AnalysisManager::deliverProgress(
    const double value,
    const quint64 count,
    const quint64 epoch)
{
    if (impl_->acceptsEpoch(epoch)) {
        emit progressChanged(std::clamp(value, 0.0, 1.0), count);
    }
}

void AnalysisManager::deliverState(const AnalysisState state, const quint64 epoch)
{
    if (impl_->acceptsEpoch(epoch)) {
        emit stateChanged(state);
    }
}

void AnalysisManager::deliverError(QString detail, const quint64 epoch)
{
    if (impl_->acceptsEpoch(epoch)) {
        emit errorOccurred(std::move(detail));
    }
}

} // namespace vidscope::analysis
