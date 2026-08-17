#include "playback/PlaybackController.h"

#include "core/Cancellation.h"
#include "core/Logging.h"
#include "media/FrameConverter.h"

#include <QtCore/QMetaObject>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

namespace vidscope::playback {
namespace {

using SteadyClock = std::chrono::steady_clock;

[[nodiscard]] qint64 toQtNanoseconds(media::MediaTime time) noexcept
{
    const auto value = time.count();
    return static_cast<qint64>(std::clamp<std::int64_t>(
        value,
        std::numeric_limits<qint64>::min(),
        std::numeric_limits<qint64>::max()));
}

[[nodiscard]] std::filesystem::path toFilesystemPath(const QString& path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}

[[nodiscard]] QString exceptionDetail(const std::exception& exception)
{
    const auto detail = QString::fromUtf8(exception.what());
    return detail.isEmpty() ? QStringLiteral("The media operation failed without an error message.")
                            : detail;
}

[[nodiscard]] bool sameLogicalFrame(
    const media::DecodedFrame& left,
    const media::DecodedFrame& right) noexcept
{
    if (left.id.sessionSerial == right.id.sessionSerial) {
        return true;
    }
    if (left.id.presentationIndex >= 0 && right.id.presentationIndex >= 0) {
        return left.id.presentationIndex == right.id.presentationIndex;
    }

    if (left.id.pts == AV_NOPTS_VALUE || right.id.pts == AV_NOPTS_VALUE
        || left.id.pts != right.id.pts || left.presentationTime == media::kNoMediaTime
        || right.presentationTime == media::kNoMediaTime
        || left.presentationTime != right.presentationTime) {
        return false;
    }

    if (left.dts != AV_NOPTS_VALUE && right.dts != AV_NOPTS_VALUE
        && left.dts != right.dts) {
        return false;
    }
    return media::visibleImagesEqual(left, right);
}

} // namespace

class PlaybackController::Impl final {
public:
    enum class CommandType {
        Open,
        Play,
        Pause,
        Toggle,
        Stop,
        Seek,
        NextFrame,
        PreviousFrame,
        NextKeyframe,
        PreviousKeyframe,
        PlaybackTick,
    };

    Impl(PlaybackController* owner, PlaybackSessionConfig config)
        : owner_(owner)
        , deliveryEpoch_(std::make_shared<std::atomic_uint64_t>(0))
        , lifecycleEpoch_(std::make_shared<std::atomic_uint64_t>(0))
        , frameDelivery_(std::make_shared<FrameDeliveryState>())
        , worker_([this, config = std::move(config)](std::stop_token stop) mutable {
            run(stop, std::move(config));
        })
    {
    }

    ~Impl()
    {
        shutdown();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] PlaybackState state() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }

    void enqueueOpen(QString path)
    {
        if (path.isEmpty()) {
            return;
        }

        Command command;
        command.type = CommandType::Open;
        command.path = std::move(path);
        enqueue(std::move(command));
    }

    void enqueueSimple(CommandType type)
    {
        Command command;
        command.type = type;
        enqueue(std::move(command));
    }

    void enqueueFrameStep(int frameCount)
    {
        const auto boundedCount = std::clamp(
            frameCount,
            -static_cast<int>(kMaximumNavigationRepetitions),
            static_cast<int>(kMaximumNavigationRepetitions));
        if (boundedCount == 0) {
            return;
        }

        Command command;
        command.type = boundedCount > 0 ? CommandType::NextFrame : CommandType::PreviousFrame;
        command.repetitions = static_cast<std::uint32_t>(
            boundedCount > 0 ? boundedCount : -boundedCount);
        enqueue(std::move(command));
    }

    void enqueueSeek(qint64 nanoseconds)
    {
        Command command;
        command.type = CommandType::Seek;
        command.nanoseconds = std::max<qint64>(0, nanoseconds);
        enqueue(std::move(command));
    }

private:
    struct Command final {
        CommandType type = CommandType::Pause;
        QString path;
        qint64 nanoseconds = 0;
        std::uint32_t repetitions = 1;
        std::uint64_t generation = 0;
        std::uint64_t deliveryEpoch = 0;
        std::uint64_t lifecycleEpoch = 0;
    };

    struct PendingFrame final {
        media::DecodedFramePtr frame;
        QImage image;
    };

    struct GuiFrameDelivery final {
        media::DecodedFramePtr frame;
        QImage image;
        std::uint64_t epoch = 0;
        double decodeFramesPerSecond = 0.0;
        qint64 seekMicroseconds = 0;
        qsizetype cachedFrames = 0;
    };

    struct FrameDeliveryState final {
        std::mutex mutex;
        std::optional<GuiFrameDelivery> latest;
        bool callbackQueued = false;
        bool accepting = true;
    };

    struct WorkerContext final {
        explicit WorkerContext(PlaybackSessionConfig config)
            : session(std::move(config))
        {
        }

        PlaybackSession session;
        media::FrameConverter converter;
        media::DecodedFramePtr publishedFrame;
        std::optional<PendingFrame> pendingFrame;
        std::uint64_t lifecycleEpoch = 0;
        bool clockValid = false;
        SteadyClock::time_point clockWall{};
        media::MediaTime clockMedia{};
        double smoothedDecodeFps = 0.0;
        qint64 lastSeekMicroseconds = 0;
    };

    static constexpr std::size_t kMaximumQueuedCommands = 64;
    static constexpr std::uint32_t kMaximumNavigationRepetitions = 1'000;

    [[nodiscard]] static bool isNavigation(CommandType type) noexcept
    {
        return type == CommandType::NextFrame || type == CommandType::PreviousFrame
            || type == CommandType::NextKeyframe || type == CommandType::PreviousKeyframe;
    }

    [[nodiscard]] static bool invalidatesFrameDelivery(CommandType type) noexcept
    {
        return type == CommandType::Open || type == CommandType::Stop || type == CommandType::Seek
            || isNavigation(type);
    }

    [[nodiscard]] static bool shouldCancel(CommandType incoming, CommandType active) noexcept
    {
        if (incoming == CommandType::Open || incoming == CommandType::Stop) {
            return true;
        }
        if (incoming == CommandType::Seek) {
            return active != CommandType::Open;
        }
        if (incoming == CommandType::Pause || incoming == CommandType::Toggle) {
            return active == CommandType::PlaybackTick;
        }
        if (isNavigation(incoming)) {
            return active == CommandType::PlaybackTick;
        }
        return false;
    }

    void enqueue(Command command)
    {
        command.generation = nextGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (invalidatesFrameDelivery(command.type)) {
            command.deliveryEpoch = deliveryEpoch_->fetch_add(1, std::memory_order_acq_rel) + 1;
        } else {
            command.deliveryEpoch = deliveryEpoch_->load(std::memory_order_acquire);
        }
        {
            std::lock_guard lifecycleLock(lifecycleMutex_);
            if (command.type == CommandType::Open) {
                command.lifecycleEpoch =
                    lifecycleEpoch_->fetch_add(1, std::memory_order_acq_rel) + 1;
            } else {
                command.lifecycleEpoch = lifecycleEpoch_->load(std::memory_order_acquire);
            }
        }

        std::lock_guard lock(commandMutex_);
        if (shuttingDown_) {
            return;
        }

        if (activeCancellation_ && activeCommand_
            && shouldCancel(command.type, *activeCommand_)) {
            activeCancellation_->requestCancellation();
        }

        if (command.type == CommandType::Open || command.type == CommandType::Stop) {
            commands_.clear();
        } else if (command.type == CommandType::Seek) {
            std::erase_if(commands_, [](const Command& queued) {
                return queued.type == CommandType::Seek || isNavigation(queued.type);
            });
        } else if (command.type == CommandType::Play || command.type == CommandType::Pause) {
            std::erase_if(commands_, [](const Command& queued) {
                return queued.type == CommandType::Play || queued.type == CommandType::Pause;
            });
        } else if (isNavigation(command.type) && !commands_.empty()
                   && commands_.back().type == command.type) {
            auto& queued = commands_.back();
            queued.repetitions = std::min<std::uint32_t>(
                queued.repetitions + command.repetitions,
                kMaximumNavigationRepetitions);
            queued.generation = command.generation;
            queued.deliveryEpoch = command.deliveryEpoch;
            commandReady_.notify_one();
            return;
        }

        if (commands_.size() >= kMaximumQueuedCommands) {
            const auto removable = std::find_if(commands_.begin(), commands_.end(), [](const Command& queued) {
                return queued.type != CommandType::Open && queued.type != CommandType::Stop;
            });
            if (removable != commands_.end()) {
                commands_.erase(removable);
            } else {
                return;
            }
        }

        commands_.push_back(std::move(command));
        commandReady_.notify_one();
    }

    void shutdown() noexcept
    {
        {
            std::lock_guard lock(frameDelivery_->mutex);
            frameDelivery_->accepting = false;
            frameDelivery_->latest.reset();
        }
        {
            std::lock_guard lock(commandMutex_);
            if (shuttingDown_) {
                return;
            }
            shuttingDown_ = true;
            commands_.clear();
            if (activeCancellation_) {
                activeCancellation_->requestCancellation();
            }
        }

        worker_.request_stop();
        commandReady_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] std::shared_ptr<core::CancellationSource> beginOperation(
        CommandType type,
        std::uint64_t generation)
    {
        auto source = std::make_shared<core::CancellationSource>();
        std::lock_guard lock(commandMutex_);
        activeCancellation_ = source;
        activeCommand_ = type;
        const bool superseded = std::any_of(
            commands_.begin(),
            commands_.end(),
            [type](const Command& queued) { return shouldCancel(queued.type, type); });
        if (shuttingDown_ || superseded) {
            source->requestCancellation();
        }
        Q_UNUSED(generation);
        return source;
    }

    void endOperation(const std::shared_ptr<core::CancellationSource>& source) noexcept
    {
        std::lock_guard lock(commandMutex_);
        if (activeCancellation_ == source) {
            activeCancellation_.reset();
            activeCommand_.reset();
        }
    }

    [[nodiscard]] bool popCommand(Command& command)
    {
        std::lock_guard lock(commandMutex_);
        if (commands_.empty()) {
            return false;
        }
        command = std::move(commands_.front());
        commands_.pop_front();
        return true;
    }

    [[nodiscard]] bool hasQueuedCommand() const
    {
        std::lock_guard lock(commandMutex_);
        return !commands_.empty();
    }

    void setState(PlaybackState state)
    {
        const auto previous = state_.exchange(state, std::memory_order_acq_rel);
        if (previous == state) {
            return;
        }
        QMetaObject::invokeMethod(
            owner_,
            [owner = owner_, state] { emit owner->stateChanged(state); },
            Qt::QueuedConnection);
    }

    [[nodiscard]] bool transitionToFailureState(
        const std::uint64_t operationLifecycleEpoch)
    {
        PlaybackState previous;
        {
            std::lock_guard lifecycleLock(lifecycleMutex_);
            if (lifecycleEpoch_->load(std::memory_order_acquire)
                != operationLifecycleEpoch) {
                return false;
            }
            previous = state_.exchange(PlaybackState::Error, std::memory_order_acq_rel);
        }

        if (previous != PlaybackState::Error) {
            const auto gate = lifecycleEpoch_;
            QMetaObject::invokeMethod(
                owner_,
                [owner = owner_, gate, operationLifecycleEpoch] {
                    if (gate->load(std::memory_order_acquire)
                        == operationLifecycleEpoch) {
                        emit owner->stateChanged(PlaybackState::Error);
                    }
                },
                Qt::QueuedConnection);
        }
        return true;
    }

    void postMediaOpened(media::MediaInfoPtr info, std::uint64_t lifecycleEpoch)
    {
        const auto gate = lifecycleEpoch_;
        QMetaObject::invokeMethod(
            owner_,
            [owner = owner_, gate, lifecycleEpoch, info = std::move(info)] {
                if (gate->load(std::memory_order_acquire) != lifecycleEpoch) {
                    return;
                }
                emit owner->mediaOpened(info);
                emit owner->durationChanged(toQtNanoseconds(info->duration));
            },
            Qt::QueuedConnection);
    }

    void postMediaClosed(std::uint64_t lifecycleEpoch)
    {
        const auto gate = lifecycleEpoch_;
        QMetaObject::invokeMethod(
            owner_,
            [owner = owner_, gate, lifecycleEpoch] {
                if (gate->load(std::memory_order_acquire) != lifecycleEpoch) {
                    return;
                }
                emit owner->mediaClosed();
                emit owner->durationChanged(0);
                emit owner->positionChanged(0);
            },
            Qt::QueuedConnection);
    }

    void postFrame(
        media::DecodedFramePtr frame,
        QImage image,
        std::uint64_t deliveryEpoch,
        const WorkerContext& context)
    {
        const auto stats = context.session.cacheStats();
        const auto cachedFrames = static_cast<qsizetype>(std::min<std::size_t>(
            stats.frameCount,
            static_cast<std::size_t>(std::numeric_limits<qsizetype>::max())));
        GuiFrameDelivery delivery{
            std::move(frame),
            std::move(image),
            deliveryEpoch,
            context.smoothedDecodeFps,
            context.lastSeekMicroseconds,
            cachedFrames,
        };

        const auto state = frameDelivery_;
        bool queueCallback = false;
        {
            std::lock_guard lock(state->mutex);
            if (!state->accepting) {
                return;
            }
            state->latest = std::move(delivery);
            if (!state->callbackQueued) {
                state->callbackQueued = true;
                queueCallback = true;
            }
        }
        if (!queueCallback) {
            return;
        }

        const auto gate = deliveryEpoch_;
        const bool queued = QMetaObject::invokeMethod(
            owner_,
            [owner = owner_, gate, state] {
                std::optional<GuiFrameDelivery> current;
                {
                    std::lock_guard lock(state->mutex);
                    if (!state->accepting) {
                        state->latest.reset();
                        state->callbackQueued = false;
                        return;
                    }
                    current = std::move(state->latest);
                    state->latest.reset();
                    state->callbackQueued = false;
                }

                if (!current
                    || gate->load(std::memory_order_acquire) != current->epoch) {
                    return;
                }
                emit owner->frameReady(current->frame, current->image);
                emit owner->positionChanged(toQtNanoseconds(current->frame->presentationTime));
                emit owner->metricsUpdated(
                    current->decodeFramesPerSecond,
                    current->seekMicroseconds,
                    current->cachedFrames);
            },
            Qt::QueuedConnection);
        if (!queued) {
            std::lock_guard lock(state->mutex);
            state->latest.reset();
            state->callbackQueued = false;
        }
    }

    void postError(QString title, QString detail, std::uint64_t lifecycleEpoch)
    {
        const auto gate = lifecycleEpoch_;
        QMetaObject::invokeMethod(
            owner_,
            [owner = owner_, gate, lifecycleEpoch, title = std::move(title), detail = std::move(detail)] {
                if (gate->load(std::memory_order_acquire) == lifecycleEpoch) {
                    emit owner->errorOccurred(title, detail);
                }
            },
            Qt::QueuedConnection);
    }

    void updateDecodeRate(WorkerContext& context, SteadyClock::duration elapsed)
    {
        const auto seconds = std::chrono::duration<double>(elapsed).count();
        if (seconds <= 0.0) {
            return;
        }
        const auto instantaneous = 1.0 / seconds;
        context.smoothedDecodeFps = context.smoothedDecodeFps == 0.0
            ? instantaneous
            : (context.smoothedDecodeFps * 0.85) + (instantaneous * 0.15);
    }

    [[nodiscard]] bool convertFrame(
        WorkerContext& context,
        PendingFrame& pending,
        CommandType operation,
        std::uint64_t generation)
    {
        if (!pending.image.isNull()) {
            return true;
        }

        const auto cancellation = beginOperation(operation, generation);
        try {
            pending.image = context.converter.toBgraImage(*pending.frame, cancellation->token());
        } catch (...) {
            endOperation(cancellation);
            throw;
        }
        endOperation(cancellation);
        return !pending.image.isNull();
    }

    void publishPending(
        WorkerContext& context,
        CommandType operation,
        std::uint64_t generation,
        std::uint64_t deliveryEpoch)
    {
        if (!context.pendingFrame) {
            return;
        }
        if (!convertFrame(context, *context.pendingFrame, operation, generation)) {
            return;
        }

        auto pending = std::move(*context.pendingFrame);
        context.pendingFrame.reset();
        context.publishedFrame = pending.frame;
        postFrame(
            std::move(pending.frame),
            std::move(pending.image),
            deliveryEpoch,
            context);
    }

    void publishResult(
        WorkerContext& context,
        const NavigationResult& result,
        CommandType operation,
        std::uint64_t generation,
        std::uint64_t deliveryEpoch)
    {
        if (!result) {
            return;
        }
        context.pendingFrame = PendingFrame{result.frame, {}};
        publishPending(context, operation, generation, deliveryEpoch);
    }

    void resetClock(WorkerContext& context)
    {
        context.clockValid = context.publishedFrame
            && context.publishedFrame->presentationTime != media::kNoMediaTime;
        context.clockWall = SteadyClock::now();
        context.clockMedia = context.clockValid
            ? context.publishedFrame->presentationTime
            : media::MediaTime{};
    }

    [[nodiscard]] media::MediaTime fallbackFrameDuration(const WorkerContext& context) const
    {
        if (const auto* info = context.session.mediaInfo()) {
            const auto nominal = media::nominalFrameDuration(info->averageFrameRate);
            if (nominal > media::MediaTime::zero()) {
                return nominal;
            }
        }
        return std::chrono::milliseconds(40);
    }

    [[nodiscard]] media::MediaTime boundedSchedulingDuration(
        const WorkerContext& context) const
    {
        constexpr auto kDefaultDuration = std::chrono::milliseconds(40);
        constexpr auto kMaximumSaneDuration = std::chrono::hours(24);

        auto duration = context.publishedFrame
            ? context.publishedFrame->duration
            : media::MediaTime::zero();
        if (duration <= media::MediaTime::zero()) {
            duration = fallbackFrameDuration(context);
        }
        if (duration <= media::MediaTime::zero()
            || duration > kMaximumSaneDuration) {
            return kDefaultDuration;
        }
        return duration;
    }

    [[nodiscard]] SteadyClock::time_point pendingDeadline(const WorkerContext& context) const
    {
        if (!context.publishedFrame || !context.pendingFrame
            || !context.pendingFrame->frame) {
            return SteadyClock::now();
        }

        const auto frameDuration = boundedSchedulingDuration(context);
        const auto fallbackDeadline = [&] {
            return SteadyClock::now()
                + std::chrono::duration_cast<SteadyClock::duration>(frameDuration);
        };
        if (!context.clockValid
            || context.clockMedia == media::kNoMediaTime
            || context.publishedFrame->presentationTime == media::kNoMediaTime
            || context.pendingFrame->frame->presentationTime == media::kNoMediaTime) {
            return fallbackDeadline();
        }

        auto mediaOffset = context.pendingFrame->frame->presentationTime - context.clockMedia;
        if (mediaOffset <= media::MediaTime::zero()) {
            mediaOffset = (context.publishedFrame->presentationTime - context.clockMedia)
                + frameDuration;
        }

        constexpr auto kMaximumSaneOffset = std::chrono::hours(24);
        if (mediaOffset < media::MediaTime::zero() || mediaOffset > kMaximumSaneOffset) {
            return fallbackDeadline();
        }
        return context.clockWall
            + std::chrono::duration_cast<SteadyClock::duration>(mediaOffset);
    }

    [[nodiscard]] bool preparePlaybackFrame(
        WorkerContext& context,
        std::uint64_t generation)
    {
        if (context.pendingFrame) {
            return true;
        }

        const auto cancellation = beginOperation(CommandType::PlaybackTick, generation);
        const auto started = SteadyClock::now();
        NavigationResult result;
        try {
            result = context.session.nextFrame(cancellation->token());
        } catch (...) {
            endOperation(cancellation);
            throw;
        }
        updateDecodeRate(context, SteadyClock::now() - started);

        if (result) {
            context.pendingFrame = PendingFrame{result.frame, {}};
            try {
                context.pendingFrame->image = context.converter.toBgraImage(
                    *result.frame,
                    cancellation->token());
            } catch (...) {
                endOperation(cancellation);
                throw;
            }
        }
        endOperation(cancellation);

        if (result.status == NavigationStatus::EndOfStream) {
            setState(PlaybackState::Ended);
            context.clockValid = false;
            return false;
        }
        return static_cast<bool>(context.pendingFrame);
    }

    [[nodiscard]] bool synchronizeSessionToPublished(
        WorkerContext& context,
        const Command& command,
        const std::shared_ptr<core::CancellationSource>& cancellation)
    {
        if (!context.pendingFrame) {
            return true;
        }
        context.pendingFrame.reset();
        if (!context.publishedFrame) {
            return true;
        }

        const auto previous = context.session.previousFrame(cancellation->token());
        if (previous && sameLogicalFrame(*previous.frame, *context.publishedFrame)) {
            return true;
        }

        SeekRequest request;
        request.generation = command.generation;
        request.target = context.publishedFrame->presentationTime;
        request.bias = SeekBias::AtOrAfter;
        const auto restored = context.session.seek(request, cancellation->token());
        return restored && sameLogicalFrame(*restored.frame, *context.publishedFrame);
    }

    void handleOpen(WorkerContext& context, const Command& command)
    {
        context.pendingFrame.reset();
        context.publishedFrame.reset();
        context.clockValid = false;
        context.converter.reset();

        if (context.session.isOpen()) {
            context.session.close();
            postMediaClosed(command.lifecycleEpoch);
        }
        context.lifecycleEpoch = command.lifecycleEpoch;
        setState(PlaybackState::Closed);

        const auto cancellation = beginOperation(CommandType::Open, command.generation);
        NavigationResult result;
        try {
            result = context.session.open(toFilesystemPath(command.path), cancellation->token());
        } catch (...) {
            endOperation(cancellation);
            throw;
        }

        if (cancellation->isCancellationRequested()) {
            endOperation(cancellation);
            return;
        }

        const auto* sourceInfo = context.session.mediaInfo();
        if (!sourceInfo) {
            endOperation(cancellation);
            if (transitionToFailureState(command.lifecycleEpoch)) {
                postError(
                    QStringLiteral("Unable to open media"),
                    QStringLiteral(
                        "No decodable video stream was found in the selected file."),
                    command.lifecycleEpoch);
            }
            return;
        }

        auto info = std::make_shared<const media::MediaInfo>(*sourceInfo);
        postMediaOpened(info, command.lifecycleEpoch);

        if (result) {
            context.pendingFrame = PendingFrame{result.frame, {}};
            try {
                context.pendingFrame->image = context.converter.toBgraImage(
                    *result.frame,
                    cancellation->token());
            } catch (...) {
                endOperation(cancellation);
                throw;
            }
        }
        endOperation(cancellation);

        if (result) {
            publishPending(
                context,
                CommandType::Open,
                command.generation,
                command.deliveryEpoch);
            setState(PlaybackState::Stopped);
            qCInfo(logPlayer) << "Opened" << command.path
                              << (context.session.usesHardwareAcceleration() ? "with hardware decode"
                                                                           : "with software decode");
        } else if (result.status == NavigationStatus::EndOfStream) {
            setState(PlaybackState::Ended);
        } else if (result.status != NavigationStatus::Cancelled) {
            if (transitionToFailureState(command.lifecycleEpoch)) {
                postError(
                    QStringLiteral("Unable to decode media"),
                    QStringLiteral(
                        "The video stream opened, but its first frame could not be decoded."),
                    command.lifecycleEpoch);
            }
        }
    }

    void handleSeek(WorkerContext& context, const Command& command)
    {
        if (!context.session.isOpen()) {
            return;
        }

        const bool resumePlayback = state() == PlaybackState::Playing;
        context.pendingFrame.reset();
        context.clockValid = false;

        SeekRequest request;
        request.generation = command.generation;
        request.target = media::MediaTime(command.nanoseconds);
        request.bias = SeekBias::AtOrAfter;

        const auto cancellation = beginOperation(CommandType::Seek, command.generation);
        const auto started = SteadyClock::now();
        NavigationResult result;
        try {
            result = context.session.seek(request, cancellation->token());
        } catch (...) {
            endOperation(cancellation);
            throw;
        }
        context.lastSeekMicroseconds = static_cast<qint64>(
            std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - started).count());
        endOperation(cancellation);

        if (cancellation->isCancellationRequested()) {
            return;
        }
        if (result) {
            publishResult(
                context,
                result,
                CommandType::Seek,
                command.generation,
                command.deliveryEpoch);
            setState(resumePlayback ? PlaybackState::Playing : PlaybackState::Paused);
            if (resumePlayback) {
                resetClock(context);
            }
        } else if (result.status == NavigationStatus::EndOfStream) {
            setState(PlaybackState::Ended);
        }
    }

    void handleStop(WorkerContext& context, const Command& command)
    {
        context.pendingFrame.reset();
        context.clockValid = false;
        if (!context.session.isOpen()) {
            setState(PlaybackState::Closed);
            return;
        }

        SeekRequest request;
        request.generation = command.generation;
        request.target = media::MediaTime::zero();
        request.bias = SeekBias::AtOrAfter;

        const auto cancellation = beginOperation(CommandType::Stop, command.generation);
        NavigationResult result;
        try {
            result = context.session.seek(request, cancellation->token());
        } catch (...) {
            endOperation(cancellation);
            throw;
        }
        endOperation(cancellation);
        if (cancellation->isCancellationRequested()) {
            return;
        }
        if (result) {
            publishResult(
                context,
                result,
                CommandType::Stop,
                command.generation,
                command.deliveryEpoch);
        }
        setState(PlaybackState::Stopped);
    }

    void handlePlay(WorkerContext& context, const Command& command)
    {
        if (!context.session.isOpen()) {
            return;
        }
        if (state() == PlaybackState::Ended) {
            handleStop(context, command);
        }
        setState(PlaybackState::Playing);
        resetClock(context);
    }

    void handleNavigation(WorkerContext& context, const Command& command)
    {
        if (!context.session.isOpen()) {
            return;
        }

        setState(PlaybackState::Paused);
        context.clockValid = false;
        const auto cancellation = beginOperation(command.type, command.generation);
        NavigationResult lastResult;
        bool reachedEnd = false;

        try {
            std::uint32_t completed = 0;
            if (command.type == CommandType::NextFrame && context.pendingFrame) {
                lastResult = NavigationResult{NavigationStatus::FrameReady, context.pendingFrame->frame};
                context.pendingFrame.reset();
                ++completed;
            } else if (!synchronizeSessionToPublished(context, command, cancellation)) {
                endOperation(cancellation);
                return;
            }

            for (; completed < command.repetitions && !cancellation->isCancellationRequested();
                 ++completed) {
                NavigationResult result;
                switch (command.type) {
                case CommandType::NextFrame:
                    result = context.session.nextFrame(cancellation->token());
                    break;
                case CommandType::PreviousFrame:
                    result = context.session.previousFrame(cancellation->token());
                    break;
                case CommandType::NextKeyframe:
                    result = context.session.nextKeyframe(cancellation->token());
                    break;
                case CommandType::PreviousKeyframe:
                    result = context.session.previousKeyframe(cancellation->token());
                    break;
                default:
                    break;
                }
                if (result) {
                    lastResult = std::move(result);
                    continue;
                }
                reachedEnd = command.type == CommandType::NextFrame
                    && result.status == NavigationStatus::EndOfStream;
                break;
            }
        } catch (...) {
            endOperation(cancellation);
            throw;
        }
        endOperation(cancellation);

        if (cancellation->isCancellationRequested()) {
            return;
        }
        if (lastResult) {
            publishResult(
                context,
                lastResult,
                command.type,
                command.generation,
                command.deliveryEpoch);
        }
        setState(reachedEnd ? PlaybackState::Ended : PlaybackState::Paused);
    }

    void handleCommand(WorkerContext& context, const Command& command)
    {
        switch (command.type) {
        case CommandType::Open:
            handleOpen(context, command);
            break;
        case CommandType::Play:
            handlePlay(context, command);
            break;
        case CommandType::Pause:
            if (state() == PlaybackState::Playing) {
                setState(PlaybackState::Paused);
                context.clockValid = false;
            }
            break;
        case CommandType::Toggle:
            if (state() == PlaybackState::Playing) {
                setState(PlaybackState::Paused);
                context.clockValid = false;
            } else {
                handlePlay(context, command);
            }
            break;
        case CommandType::Stop:
            handleStop(context, command);
            break;
        case CommandType::Seek:
            handleSeek(context, command);
            break;
        case CommandType::NextFrame:
        case CommandType::PreviousFrame:
        case CommandType::NextKeyframe:
        case CommandType::PreviousKeyframe:
            handleNavigation(context, command);
            break;
        case CommandType::PlaybackTick:
            break;
        }
    }

    void handleFailure(
        WorkerContext& context,
        const std::exception& exception,
        const std::uint64_t operationLifecycleEpoch)
    {
        qCCritical(logPlayer) << "Playback worker failure:" << exception.what();
        context.pendingFrame.reset();
        context.publishedFrame.reset();
        context.clockValid = false;
        context.converter.reset();
        context.session.close();
        context.lifecycleEpoch = 0;

        if (!transitionToFailureState(operationLifecycleEpoch)) {
            qCInfo(logPlayer)
                << "Suppressed failure notification from superseded media lifecycle"
                << operationLifecycleEpoch;
            return;
        }

        postMediaClosed(operationLifecycleEpoch);
        postError(
            QStringLiteral("Playback error"),
            exceptionDetail(exception),
            operationLifecycleEpoch);
    }

    void handleUnknownFailure(
        WorkerContext& context,
        const std::uint64_t operationLifecycleEpoch)
    {
        qCCritical(logPlayer) << "Playback worker failed with an unknown exception";
        context.pendingFrame.reset();
        context.publishedFrame.reset();
        context.clockValid = false;
        context.converter.reset();
        context.session.close();
        context.lifecycleEpoch = 0;

        if (!transitionToFailureState(operationLifecycleEpoch)) {
            qCInfo(logPlayer)
                << "Suppressed unknown failure notification from superseded media lifecycle"
                << operationLifecycleEpoch;
            return;
        }

        postMediaClosed(operationLifecycleEpoch);
        postError(
            QStringLiteral("Playback error"),
            QStringLiteral("An unexpected error occurred while decoding the video."),
            operationLifecycleEpoch);
    }

    void run(std::stop_token stop, PlaybackSessionConfig config)
    {
        WorkerContext context(std::move(config));
        qCInfo(logPlayer) << "Playback worker started";

        while (!stop.stop_requested()) {
            Command command;
            if (popCommand(command)) {
                try {
                    handleCommand(context, command);
                } catch (const std::exception& exception) {
                    handleFailure(context, exception, command.lifecycleEpoch);
                } catch (...) {
                    handleUnknownFailure(context, command.lifecycleEpoch);
                }
                continue;
            }

            if (state() == PlaybackState::Playing && context.session.isOpen()) {
                const auto generation = nextGeneration_.load(std::memory_order_acquire);
                const auto operationLifecycleEpoch = context.lifecycleEpoch;
                try {
                    if (!preparePlaybackFrame(context, generation)) {
                        continue;
                    }
                } catch (const std::exception& exception) {
                    handleFailure(context, exception, operationLifecycleEpoch);
                    continue;
                } catch (...) {
                    handleUnknownFailure(context, operationLifecycleEpoch);
                    continue;
                }

                const auto deadline = pendingDeadline(context);
                std::unique_lock lock(commandMutex_);
                commandReady_.wait_until(lock, deadline, [this, &stop] {
                    return stop.stop_requested() || shuttingDown_ || !commands_.empty();
                });
                const bool interrupted = stop.stop_requested() || shuttingDown_ || !commands_.empty();
                lock.unlock();
                if (interrupted) {
                    continue;
                }

                try {
                    publishPending(
                        context,
                        CommandType::PlaybackTick,
                        generation,
                        deliveryEpoch_->load(std::memory_order_acquire));
                } catch (const std::exception& exception) {
                    handleFailure(context, exception, operationLifecycleEpoch);
                } catch (...) {
                    handleUnknownFailure(context, operationLifecycleEpoch);
                }
                continue;
            }

            std::unique_lock lock(commandMutex_);
            commandReady_.wait(lock, [this, &stop] {
                return stop.stop_requested() || shuttingDown_ || !commands_.empty();
            });
        }

        {
            std::lock_guard lock(commandMutex_);
            if (activeCancellation_) {
                activeCancellation_->requestCancellation();
            }
        }
        context.pendingFrame.reset();
        context.publishedFrame.reset();
        context.converter.reset();
        context.session.close();
        qCInfo(logPlayer) << "Playback worker stopped";
    }

    PlaybackController* const owner_;
    std::atomic<PlaybackState> state_{PlaybackState::Closed};
    std::atomic_uint64_t nextGeneration_{0};
    std::shared_ptr<std::atomic_uint64_t> deliveryEpoch_;
    std::shared_ptr<std::atomic_uint64_t> lifecycleEpoch_;
    std::shared_ptr<FrameDeliveryState> frameDelivery_;

    mutable std::mutex lifecycleMutex_;
    mutable std::mutex commandMutex_;
    std::condition_variable commandReady_;
    std::deque<Command> commands_;
    std::shared_ptr<core::CancellationSource> activeCancellation_;
    std::optional<CommandType> activeCommand_;
    bool shuttingDown_ = false;
    std::jthread worker_;
};

PlaybackController::PlaybackController(PlaybackSessionConfig config, QObject* parent)
    : QObject(parent)
    , impl_(std::make_unique<Impl>(this, std::move(config)))
{
    qRegisterMetaType<media::MediaInfoPtr>();
    qRegisterMetaType<media::DecodedFramePtr>();
    qRegisterMetaType<PlaybackState>();
}

PlaybackController::~PlaybackController() = default;

PlaybackState PlaybackController::state() const noexcept
{
    return impl_->state();
}

void PlaybackController::openFile(const QString& path)
{
    impl_->enqueueOpen(path);
}

void PlaybackController::play()
{
    impl_->enqueueSimple(Impl::CommandType::Play);
}

void PlaybackController::pause()
{
    impl_->enqueueSimple(Impl::CommandType::Pause);
}

void PlaybackController::togglePlayPause()
{
    impl_->enqueueSimple(Impl::CommandType::Toggle);
}

void PlaybackController::stop()
{
    impl_->enqueueSimple(Impl::CommandType::Stop);
}

void PlaybackController::seekToNanoseconds(qint64 nanoseconds)
{
    impl_->enqueueSeek(nanoseconds);
}

void PlaybackController::stepFrames(int frameCount)
{
    impl_->enqueueFrameStep(frameCount);
}

void PlaybackController::nextFrame()
{
    stepFrames(1);
}

void PlaybackController::previousFrame()
{
    stepFrames(-1);
}

void PlaybackController::nextKeyframe()
{
    impl_->enqueueSimple(Impl::CommandType::NextKeyframe);
}

void PlaybackController::previousKeyframe()
{
    impl_->enqueueSimple(Impl::CommandType::PreviousKeyframe);
}

} // namespace vidscope::playback
