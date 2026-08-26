#include "playback/PlaybackSession.h"

#include "media/Demuxer.h"
#include "media/FfmpegRaii.h"
#include "media/MediaSource.h"
#include "playback/FrameQueue.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_map>
#include <utility>

extern "C" {
#include <libavcodec/packet.h>
}

namespace vidscope::playback {
namespace {

[[nodiscard]] bool presentationBefore(
    const media::DecodedFrame& left,
    const media::DecodedFrame& right) noexcept
{
    if (left.presentationTime != right.presentationTime) {
        return left.presentationTime < right.presentationTime;
    }
    return left.id.sessionSerial < right.id.sessionSerial;
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

[[nodiscard]] bool beforeReconstructionTarget(
    const media::DecodedFrame& candidate,
    const media::DecodedFrame& original) noexcept
{
    if (candidate.id.presentationIndex >= 0 && original.id.presentationIndex >= 0) {
        return candidate.id.presentationIndex < original.id.presentationIndex;
    }
    if (candidate.presentationTime == media::kNoMediaTime
        || original.presentationTime == media::kNoMediaTime) {
        return false;
    }
    // Equal-time distinct images are real predecessors until the original image is found.
    return candidate.presentationTime <= original.presentationTime;
}

[[nodiscard]] media::MediaTime clampedTarget(
    const media::MediaInfo& info,
    media::MediaTime target) noexcept
{
    target = std::max(target, media::MediaTime::zero());
    if (info.duration > media::MediaTime::zero()) {
        target = std::min(target, info.duration);
    }
    return target;
}

} // namespace

class PlaybackSession::Impl final {
public:
    explicit Impl(PlaybackSessionConfig config)
        : config_(std::move(config))
        , cache_(config_.frameCacheBytes)
    {
    }

    ~Impl()
    {
        close();
    }

    NavigationResult open(
        const std::filesystem::path& path,
        const core::CancellationToken cancellation)
    {
        close();
        if (cancellation.isCancellationRequested()) {
            return cancelled();
        }

        const auto generation = operationGate_.next();
        try {
            source_ = media::MediaSource::open(path, {}, cancellation);
            if (cancellation.isCancellationRequested() || !operationGate_.accepts(generation)) {
                close();
                return cancelled();
            }
            demuxer_ = std::make_unique<media::Demuxer>(*source_);
            decoder_ = media::VideoDecoder::create(*source_, config_.decoder, cancellation);
            packet_ = media::makePacket();
            forwardQueue_ = std::make_unique<FrameQueue>(
                config_.forwardQueueFrames,
                config_.forwardQueueBytes);
        } catch (...) {
            if (cancellation.isCancellationRequested()) {
                close();
                return cancelled();
            }
            close();
            throw;
        }

        resetDecodeState(true);
        const auto decoded = decodeOne(cancellation, generation);
        if (decoded.status == NavigationStatus::Cancelled) {
            close();
            return decoded;
        }
        if (!decoded) {
            positionValid_ = true;
            return decoded;
        }

        const auto first = decoded.frame;
        prefetchTo(config_.initialPrefetchFrames, cancellation, generation);
        if (cancellation.isCancellationRequested() || !operationGate_.accepts(generation)) {
            close();
            return cancelled();
        }
        setCurrent(first);
        positionValid_ = true;
        return ready(first);
    }

    void close() noexcept
    {
        (void)operationGate_.next();
        if (forwardQueue_) {
            forwardQueue_->close();
            forwardQueue_.reset();
        }
        headFrame_.reset();
        tailFrame_.reset();
        current_.reset();
        cache_.clear();
        presentationIndexAnchors_.clear();
        presentationIndexAnchorOrder_.clear();
        nextPresentationIndex_.reset();
        if (packet_) {
            av_packet_unref(packet_.get());
            packet_.reset();
        }
        decoder_.reset();
        demuxer_.reset();
        source_.reset();
        packetPending_ = false;
        inputEnded_ = false;
        drainSubmitted_ = false;
        decoderEnded_ = false;
        positionValid_ = false;
        lastExternalGeneration_ = 0;
    }

    NavigationResult seek(
        SeekRequest request,
        const core::CancellationToken cancellation)
    {
        if (!isOpen()) {
            return noMedia();
        }
        if (cancellation.isCancellationRequested()) {
            return cancelled();
        }
        if (request.generation != 0 && lastExternalGeneration_ != 0
            && request.generation < lastExternalGeneration_) {
            return cancelled();
        }
        if (request.generation != 0) {
            lastExternalGeneration_ = request.generation;
        }

        const auto generation = operationGate_.next();
        const auto oldCurrent = current_;
        const auto result = seekCore(request, cancellation, generation, true);
        if (result.status == NavigationStatus::Cancelled) {
            current_ = oldCurrent;
            if (current_) {
                cache_.pin(current_->id.sessionSerial);
            }
            positionValid_ = false;
        }
        return result;
    }

    NavigationResult nextFrame(const core::CancellationToken cancellation)
    {
        if (!isOpen()) {
            return noMedia();
        }
        if (cancellation.isCancellationRequested()) {
            return cancelled();
        }

        const auto generation = operationGate_.next();
        if (!positionValid_ && current_) {
            if (const auto cached = cache_.next(*current_)) {
                setCurrent(cached);
                positionValid_ = false;
                return ready(cached);
            }
        }
        if (!restorePosition(cancellation, generation)) {
            return cancellation.isCancellationRequested() ? cancelled() : endOfStream();
        }
        return takeNext(cancellation, generation);
    }

    NavigationResult previousFrame(const core::CancellationToken cancellation)
    {
        if (!isOpen()) {
            return noMedia();
        }
        if (!current_) {
            return beginning();
        }
        if (cancellation.isCancellationRequested()) {
            return cancelled();
        }

        const auto generation = operationGate_.next();
        if (const auto cached = cache_.previous(*current_)) {
            setCurrent(cached);
            positionValid_ = false;
            return ready(cached);
        }
        if (current_->id.presentationIndex == 0) {
            return beginning();
        }
        return reconstructPrevious(false, cancellation, generation);
    }

    NavigationResult nextKeyframe(const core::CancellationToken cancellation)
    {
        if (!isOpen()) {
            return noMedia();
        }
        if (cancellation.isCancellationRequested()) {
            return cancelled();
        }
        const auto generation = operationGate_.next();
        if (!restorePosition(cancellation, generation)) {
            return cancellation.isCancellationRequested() ? cancelled() : endOfStream();
        }

        const auto original = current_;
        for (;;) {
            const auto candidate = takeNext(cancellation, generation);
            if (candidate && candidate.frame->keyFrame) {
                return candidate;
            }
            if (!candidate) {
                // Scanning must not move the externally visible playhead when no
                // future keyframe exists. Decoder continuity is restored lazily.
                current_ = original;
                if (current_) {
                    cache_.pin(current_->id.sessionSerial);
                }
                positionValid_ = false;
                return candidate;
            }
        }
    }

    NavigationResult previousKeyframe(const core::CancellationToken cancellation)
    {
        if (!isOpen()) {
            return noMedia();
        }
        if (!current_) {
            return beginning();
        }
        if (cancellation.isCancellationRequested()) {
            return cancelled();
        }

        const auto generation = operationGate_.next();
        if (const auto cached = cache_.previousKeyframe(*current_)) {
            setCurrent(cached);
            positionValid_ = false;
            return ready(cached);
        }
        if (current_->id.presentationIndex == 0) {
            return beginning();
        }
        return reconstructPrevious(true, cancellation, generation);
    }

    void prefetch(const core::CancellationToken cancellation)
    {
        if (!isOpen() || cancellation.isCancellationRequested()) {
            return;
        }
        const auto generation = operationGate_.next();
        prefetchTo(config_.forwardQueueFrames, cancellation, generation);
        if (cancellation.isCancellationRequested()) {
            positionValid_ = false;
        }
    }

    [[nodiscard]] bool isOpen() const noexcept
    {
        return source_ && demuxer_ && decoder_ && packet_ && forwardQueue_;
    }

    [[nodiscard]] const media::MediaInfo* mediaInfo() const noexcept
    {
        return source_ ? &source_->info() : nullptr;
    }

    [[nodiscard]] media::DecodedFramePtr currentFrame() const noexcept
    {
        return current_;
    }

    [[nodiscard]] FrameCacheStats cacheStats() const
    {
        return cache_.stats();
    }

    [[nodiscard]] std::size_t bufferedFrameCount() const
    {
        return bufferedFrames();
    }

    [[nodiscard]] bool usesHardwareAcceleration() const noexcept
    {
        return decoder_ && decoder_->usesHardwareAcceleration();
    }

    [[nodiscard]] std::string hardwareDeviceName() const
    {
        return decoder_ ? decoder_->hardwareDeviceName() : std::string{};
    }

private:
    [[nodiscard]] static NavigationResult ready(media::DecodedFramePtr frame)
    {
        return {NavigationStatus::FrameReady, std::move(frame)};
    }

    [[nodiscard]] static NavigationResult beginning()
    {
        return {NavigationStatus::BeginningOfStream, {}};
    }

    [[nodiscard]] static NavigationResult endOfStream()
    {
        return {NavigationStatus::EndOfStream, {}};
    }

    [[nodiscard]] static NavigationResult cancelled()
    {
        return {NavigationStatus::Cancelled, {}};
    }

    [[nodiscard]] static NavigationResult noMedia()
    {
        return {NavigationStatus::NoMedia, {}};
    }

    void resetDecodeState(const bool startsAtOrigin)
    {
        if (decoder_) {
            decoder_->flush();
        }
        if (packet_) {
            av_packet_unref(packet_.get());
        }
        packetPending_ = false;
        inputEnded_ = false;
        drainSubmitted_ = false;
        decoderEnded_ = false;
        nextPresentationIndex_ = startsAtOrigin ? std::optional<std::int64_t>{0}
                                                : std::nullopt;
        if (forwardQueue_) {
            forwardQueue_->clear();
        }
        headFrame_.reset();
        tailFrame_.reset();
    }

    struct PresentationAnchorKey final {
        std::int64_t presentationNanoseconds = 0;
        std::int64_t pts = AV_NOPTS_VALUE;

        friend bool operator==(const PresentationAnchorKey&, const PresentationAnchorKey&) = default;
    };

    struct PresentationAnchorKeyHash final {
        [[nodiscard]] std::size_t operator()(const PresentationAnchorKey& key) const noexcept
        {
            const auto first = std::hash<std::int64_t>{}(key.presentationNanoseconds);
            const auto second = std::hash<std::int64_t>{}(key.pts);
            return first
                ^ (second + std::size_t{0x9e3779b9U} + (first << 6U) + (first >> 2U));
        }
    };

    struct PresentationIndexAnchor final {
        std::int64_t index = -1;
        bool ambiguous = false;
    };

    [[nodiscard]] static std::optional<PresentationAnchorKey> anchorKey(
        const media::DecodedFrame& frame) noexcept
    {
        if (frame.presentationTime == media::kNoMediaTime) {
            return std::nullopt;
        }
        return PresentationAnchorKey{frame.presentationTime.count(), frame.id.pts};
    }

    [[nodiscard]] std::optional<std::int64_t> findPresentationIndexAnchor(
        const media::DecodedFrame& frame) const
    {
        const auto key = anchorKey(frame);
        if (!key) {
            return std::nullopt;
        }
        const auto found = presentationIndexAnchors_.find(*key);
        if (found == presentationIndexAnchors_.end() || found->second.ambiguous) {
            return std::nullopt;
        }
        return found->second.index;
    }

    void rememberPresentationIndexAnchor(
        const media::DecodedFrame& frame,
        const std::int64_t index)
    {
        if (config_.presentationIndexAnchorCount == 0) {
            return;
        }
        const auto key = anchorKey(frame);
        if (!key) {
            return;
        }

        if (auto found = presentationIndexAnchors_.find(*key);
            found != presentationIndexAnchors_.end()) {
            if (found->second.index != index) {
                found->second.ambiguous = true;
            }
            return;
        }

        while (presentationIndexAnchors_.size() >= config_.presentationIndexAnchorCount
               && !presentationIndexAnchorOrder_.empty()) {
            presentationIndexAnchors_.erase(presentationIndexAnchorOrder_.front());
            presentationIndexAnchorOrder_.pop_front();
        }
        presentationIndexAnchors_.emplace(
            *key,
            PresentationIndexAnchor{index, false});
        presentationIndexAnchorOrder_.push_back(*key);
    }

    [[nodiscard]] std::uint64_t nextSerial() noexcept
    {
        if (serialCounter_ == std::numeric_limits<std::uint64_t>::max()) {
            serialCounter_ = 0;
        }
        return serialCounter_ + 1;
    }

    media::DecodedFramePtr finalizeIdentity(media::DecodedFramePtr frame)
    {
        serialCounter_ = frame->id.sessionSerial;
        std::optional<std::int64_t> assignedIndex;
        if (nextPresentationIndex_) {
            assignedIndex = *nextPresentationIndex_;
        } else {
            assignedIndex = findPresentationIndexAnchor(*frame);
        }

        if (assignedIndex && frame->id.presentationIndex != *assignedIndex) {
            auto relabelled = std::make_shared<media::DecodedFrame>(*frame);
            relabelled->id.presentationIndex = *assignedIndex;
            frame = std::move(relabelled);
        }

        if (assignedIndex) {
            rememberPresentationIndexAnchor(*frame, *assignedIndex);
            if (*assignedIndex < std::numeric_limits<std::int64_t>::max()) {
                nextPresentationIndex_ = *assignedIndex + 1;
            } else {
                nextPresentationIndex_.reset();
            }
        }
        (void)cache_.insert(frame);
        return frame;
    }

    NavigationResult decodeOne(
        const core::CancellationToken cancellation,
        const RequestGeneration generation)
    {
        if (!isOpen()) {
            return noMedia();
        }
        if (decoderEnded_) {
            return endOfStream();
        }

        for (;;) {
            if (cancellation.isCancellationRequested() || !operationGate_.accepts(generation)) {
                positionValid_ = false;
                return cancelled();
            }

            const auto index = nextPresentationIndex_.value_or(-1);
            auto received = decoder_->receiveFrame(nextSerial(), index, cancellation);
            switch (received.status) {
            case media::FrameReceiveStatus::Frame:
                return ready(finalizeIdentity(std::move(received.frame)));
            case media::FrameReceiveStatus::Cancelled:
                positionValid_ = false;
                return cancelled();
            case media::FrameReceiveStatus::EndOfStream:
                decoderEnded_ = true;
                return endOfStream();
            case media::FrameReceiveStatus::NeedInput:
                break;
            }

            if (drainSubmitted_) {
                decoderEnded_ = true;
                return endOfStream();
            }

            if (packetPending_) {
                const auto sent = decoder_->sendPacket(packet_.get(), cancellation);
                switch (sent) {
                case media::PacketSendStatus::Accepted:
                    packetPending_ = false;
                    av_packet_unref(packet_.get());
                    continue;
                case media::PacketSendStatus::NeedReceive:
                    continue;
                case media::PacketSendStatus::Cancelled:
                    positionValid_ = false;
                    return cancelled();
                case media::PacketSendStatus::EndOfStream:
                    packetPending_ = false;
                    av_packet_unref(packet_.get());
                    decoderEnded_ = true;
                    return endOfStream();
                }
            }

            if (!inputEnded_) {
                const auto read = demuxer_->readNextVideoPacket(packet_.get(), cancellation);
                switch (read) {
                case media::PacketReadStatus::Packet:
                    packetPending_ = true;
                    continue;
                case media::PacketReadStatus::Cancelled:
                    positionValid_ = false;
                    return cancelled();
                case media::PacketReadStatus::EndOfFile:
                    inputEnded_ = true;
                    break;
                }
            }

            const auto drain = decoder_->sendPacket(nullptr, cancellation);
            switch (drain) {
            case media::PacketSendStatus::Accepted:
                drainSubmitted_ = true;
                break;
            case media::PacketSendStatus::NeedReceive:
                break;
            case media::PacketSendStatus::Cancelled:
                positionValid_ = false;
                return cancelled();
            case media::PacketSendStatus::EndOfStream:
                decoderEnded_ = true;
                return endOfStream();
            }
        }
    }

    void setCurrent(const media::DecodedFramePtr& frame)
    {
        current_ = frame;
        if (frame) {
            cache_.pin(frame->id.sessionSerial);
            (void)cache_.insert(frame);
            cache_.pin(frame->id.sessionSerial);
        }
    }

    [[nodiscard]] std::size_t bufferedFrames() const
    {
        return (headFrame_ ? 1U : 0U) + (forwardQueue_ ? forwardQueue_->size() : 0U)
            + (tailFrame_ ? 1U : 0U);
    }

    void prefetchTo(
        const std::size_t requestedCount,
        const core::CancellationToken cancellation,
        const RequestGeneration generation)
    {
        if (!isOpen() || tailFrame_) {
            return;
        }
        const auto target = std::min(requestedCount, config_.forwardQueueFrames);
        while (bufferedFrames() < target && !decoderEnded_) {
            const auto decoded = decodeOne(cancellation, generation);
            if (!decoded) {
                return;
            }
            if (!forwardQueue_->tryPush(decoded.frame)) {
                // Preserve the already-consumed decoder output. This single tail
                // slot is only used when a surface cannot fit the configured byte
                // budget; decoding stops immediately, so it remains bounded.
                tailFrame_ = decoded.frame;
                return;
            }
        }
    }

    void ensureHeadFrame()
    {
        if (headFrame_ || !forwardQueue_) {
            return;
        }
        if (auto queued = forwardQueue_->tryPop()) {
            headFrame_ = std::move(*queued);
            return;
        }
        if (tailFrame_) {
            headFrame_ = std::move(tailFrame_);
            tailFrame_.reset();
        }
    }

    NavigationResult takeNext(
        const core::CancellationToken cancellation,
        const RequestGeneration generation)
    {
        if (cancellation.isCancellationRequested() || !operationGate_.accepts(generation)) {
            return cancelled();
        }
        if (!current_) {
            return beginning();
        }

        ensureHeadFrame();
        while (headFrame_ && !presentationBefore(*current_, *headFrame_)) {
            headFrame_.reset();
            ensureHeadFrame();
        }
        const auto cached = cache_.next(*current_);

        media::DecodedFramePtr selected;
        if (headFrame_ && cached) {
            if (sameLogicalFrame(*headFrame_, *cached)) {
                selected = std::move(headFrame_);
                headFrame_.reset();
            } else if (presentationBefore(*cached, *headFrame_)) {
                selected = cached;
            } else {
                selected = std::move(headFrame_);
                headFrame_.reset();
            }
        } else if (headFrame_) {
            selected = std::move(headFrame_);
            headFrame_.reset();
        } else if (cached) {
            selected = cached;
        } else {
            const auto decoded = decodeOne(cancellation, generation);
            if (!decoded) {
                return decoded;
            }
            selected = decoded.frame;
        }

        prefetchTo(config_.initialPrefetchFrames, cancellation, generation);
        if (cancellation.isCancellationRequested() || !operationGate_.accepts(generation)) {
            positionValid_ = false;
            return cancelled();
        }
        setCurrent(selected);
        positionValid_ = true;
        return ready(selected);
    }

    NavigationResult seekCore(
        SeekRequest request,
        const core::CancellationToken cancellation,
        const RequestGeneration generation,
        const bool performPrefetch)
    {
        const auto* info = mediaInfo();
        if (info == nullptr) {
            return noMedia();
        }
        request.target = clampedTarget(*info, request.target);
        const auto seekPlan = SeekController::plan(*info, request);

        bool startsAtOrigin = seekPlan.startsAtStreamOrigin;
        bool positioned = startsAtOrigin
            ? demuxer_->seekToStart(cancellation)
            : demuxer_->seekToTimestamp(seekPlan.targetStreamTimestamp, cancellation);
        if (!positioned && !cancellation.isCancellationRequested() && !startsAtOrigin) {
            positioned = demuxer_->seekToStart(cancellation);
            startsAtOrigin = positioned;
        }
        if (!positioned) {
            positionValid_ = false;
            return cancellation.isCancellationRequested() ? cancelled() : endOfStream();
        }

        resetDecodeState(startsAtOrigin);
        cache_.clear();
        media::DecodedFramePtr previous;
        media::DecodedFramePtr selected;

        for (;;) {
            const auto decoded = decodeOne(cancellation, generation);
            if (decoded.status == NavigationStatus::Cancelled) {
                positionValid_ = false;
                return decoded;
            }
            if (decoded.status == NavigationStatus::EndOfStream) {
                // A clamped duration commonly lies just beyond the last frame's
                // PTS. Publish the final decoded frame for every bias rather than
                // leaving the visible playhead behind at an unrelated position.
                selected = previous;
                break;
            }
            if (!decoded) {
                return decoded;
            }

            const auto& frame = *decoded.frame;
            if (frame.presentationTime == media::kNoMediaTime) {
                if (startsAtOrigin && request.target == media::MediaTime::zero() && !previous) {
                    selected = decoded.frame;
                    break;
                }
                previous = decoded.frame;
                continue;
            }

            if (request.bias == SeekBias::AtOrAfter) {
                if (frame.presentationTime >= request.target) {
                    selected = decoded.frame;
                    break;
                }
                previous = decoded.frame;
                continue;
            }

            if (request.bias == SeekBias::AtOrBefore) {
                if (frame.presentationTime <= request.target) {
                    previous = decoded.frame;
                    continue;
                }
                selected = previous;
                headFrame_ = decoded.frame;
                break;
            }

            if (frame.presentationTime == request.target) {
                selected = decoded.frame;
                break;
            }
            if (frame.presentationTime < request.target) {
                previous = decoded.frame;
                continue;
            }
            if (!previous) {
                selected = decoded.frame;
            } else {
                const auto beforeDistance = request.target - previous->presentationTime;
                const auto afterDistance = frame.presentationTime - request.target;
                if (beforeDistance <= afterDistance) {
                    selected = previous;
                    headFrame_ = decoded.frame;
                } else {
                    selected = decoded.frame;
                }
            }
            break;
        }

        if (!selected) {
            positionValid_ = false;
            return request.bias == SeekBias::AtOrBefore ? beginning() : endOfStream();
        }
        if (performPrefetch) {
            prefetchTo(config_.initialPrefetchFrames, cancellation, generation);
        }
        if (cancellation.isCancellationRequested() || !operationGate_.accepts(generation)) {
            positionValid_ = false;
            return cancelled();
        }
        setCurrent(selected);
        positionValid_ = true;
        return ready(selected);
    }

    bool restorePosition(
        const core::CancellationToken cancellation,
        const RequestGeneration generation)
    {
        if (positionValid_) {
            return true;
        }
        if (!current_) {
            return false;
        }
        SeekRequest restore;
        restore.target = current_->presentationTime;
        restore.bias = SeekBias::AtOrAfter;
        const auto restored = seekCore(restore, cancellation, generation, false);
        return static_cast<bool>(restored);
    }

    NavigationResult reconstructPrevious(
        const bool keyframeOnly,
        const core::CancellationToken cancellation,
        const RequestGeneration generation)
    {
        const auto original = current_;
        if (!original || !mediaInfo()) {
            return beginning();
        }

        auto safeTime = original->presentationTime;
        if (const auto keyframe = cache_.previousKeyframe(*original)) {
            safeTime = keyframe->presentationTime;
        }
        auto safeTimestamp = media::mediaTimeToTimestamp(
            safeTime,
            mediaInfo()->streamStartTimestamp,
            mediaInfo()->timeBase);
        const auto origin = mediaInfo()->streamStartTimestamp;
        if (safeTimestamp == AV_NOPTS_VALUE) {
            safeTimestamp = origin;
        } else if (safeTime == original->presentationTime && safeTimestamp > origin) {
            --safeTimestamp;
        }

        bool startsAtOrigin = safeTimestamp <= origin;
        bool positioned = startsAtOrigin
            ? demuxer_->seekToStart(cancellation)
            : demuxer_->seekToTimestamp(safeTimestamp, cancellation);
        if (!positioned && !cancellation.isCancellationRequested()) {
            positioned = demuxer_->seekToStart(cancellation);
            startsAtOrigin = positioned;
        }
        if (!positioned) {
            positionValid_ = false;
            return cancellation.isCancellationRequested() ? cancelled() : beginning();
        }

        resetDecodeState(startsAtOrigin);
        cache_.clear();
        media::DecodedFramePtr candidate;
        media::DecodedFramePtr reached;
        for (;;) {
            const auto decoded = decodeOne(cancellation, generation);
            if (decoded.status == NavigationStatus::Cancelled) {
                current_ = original;
                positionValid_ = false;
                return decoded;
            }
            if (decoded.status == NavigationStatus::EndOfStream) {
                break;
            }
            if (!decoded) {
                return decoded;
            }
            if (sameLogicalFrame(*decoded.frame, *original)) {
                reached = decoded.frame;
                break;
            }
            if (!beforeReconstructionTarget(*decoded.frame, *original)) {
                reached = decoded.frame;
                break;
            }
            if (!keyframeOnly || decoded.frame->keyFrame) {
                candidate = decoded.frame;
            }
        }

        if (!candidate && !startsAtOrigin && !cancellation.isCancellationRequested()) {
            if (!demuxer_->seekToStart(cancellation)) {
                current_ = original;
                positionValid_ = false;
                return cancellation.isCancellationRequested() ? cancelled() : beginning();
            }
            resetDecodeState(true);
            cache_.clear();
            for (;;) {
                const auto decoded = decodeOne(cancellation, generation);
                if (decoded.status == NavigationStatus::Cancelled) {
                    current_ = original;
                    positionValid_ = false;
                    return decoded;
                }
                if (!decoded) {
                    break;
                }
                if (sameLogicalFrame(*decoded.frame, *original)) {
                    reached = decoded.frame;
                    break;
                }
                if (!beforeReconstructionTarget(*decoded.frame, *original)) {
                    reached = decoded.frame;
                    break;
                }
                if (!keyframeOnly || decoded.frame->keyFrame) {
                    candidate = decoded.frame;
                }
            }
        }

        if (!candidate) {
            setCurrent(original);
            // The decoder may have emitted a new copy of the original while
            // proving that no predecessor exists. Restore before navigating so
            // that copy cannot appear as a duplicate next frame.
            positionValid_ = false;
            return beginning();
        }
        if (reached) {
            headFrame_ = reached;
        } else {
            headFrame_ = original;
            (void)cache_.insert(original);
        }
        setCurrent(candidate);
        // A frame predecessor is adjacent to reached/headFrame_. A keyframe
        // predecessor may be separated by evicted frames, so forward decode must
        // restore lazily before it can advance.
        positionValid_ = !keyframeOnly;
        return ready(candidate);
    }

    PlaybackSessionConfig config_;
    FrameCache cache_;
    std::unique_ptr<media::MediaSource> source_;
    std::unique_ptr<media::Demuxer> demuxer_;
    std::unique_ptr<media::VideoDecoder> decoder_;
    std::unique_ptr<FrameQueue> forwardQueue_;
    media::PacketPtr packet_;
    media::DecodedFramePtr current_;
    media::DecodedFramePtr headFrame_;
    media::DecodedFramePtr tailFrame_;
    std::unordered_map<
        PresentationAnchorKey,
        PresentationIndexAnchor,
        PresentationAnchorKeyHash>
        presentationIndexAnchors_;
    std::deque<PresentationAnchorKey> presentationIndexAnchorOrder_;
    std::optional<std::int64_t> nextPresentationIndex_;
    RequestGate operationGate_;
    RequestGeneration lastExternalGeneration_ = 0;
    std::uint64_t serialCounter_ = 0;
    bool packetPending_ = false;
    bool inputEnded_ = false;
    bool drainSubmitted_ = false;
    bool decoderEnded_ = false;
    bool positionValid_ = false;
};

PlaybackSession::PlaybackSession(PlaybackSessionConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

PlaybackSession::~PlaybackSession() = default;

NavigationResult PlaybackSession::open(
    const std::filesystem::path& path,
    core::CancellationToken cancellation)
{
    return impl_->open(path, std::move(cancellation));
}

void PlaybackSession::close() noexcept
{
    impl_->close();
}

NavigationResult PlaybackSession::seek(
    const SeekRequest& request,
    core::CancellationToken cancellation)
{
    return impl_->seek(request, std::move(cancellation));
}

NavigationResult PlaybackSession::nextFrame(core::CancellationToken cancellation)
{
    return impl_->nextFrame(std::move(cancellation));
}

NavigationResult PlaybackSession::previousFrame(core::CancellationToken cancellation)
{
    return impl_->previousFrame(std::move(cancellation));
}

NavigationResult PlaybackSession::nextKeyframe(core::CancellationToken cancellation)
{
    return impl_->nextKeyframe(std::move(cancellation));
}

NavigationResult PlaybackSession::previousKeyframe(core::CancellationToken cancellation)
{
    return impl_->previousKeyframe(std::move(cancellation));
}

void PlaybackSession::prefetch(core::CancellationToken cancellation)
{
    impl_->prefetch(std::move(cancellation));
}

bool PlaybackSession::isOpen() const noexcept
{
    return impl_->isOpen();
}

const media::MediaInfo* PlaybackSession::mediaInfo() const noexcept
{
    return impl_->mediaInfo();
}

media::DecodedFramePtr PlaybackSession::currentFrame() const noexcept
{
    return impl_->currentFrame();
}

FrameCacheStats PlaybackSession::cacheStats() const
{
    return impl_->cacheStats();
}

std::size_t PlaybackSession::bufferedFrames() const
{
    return impl_->bufferedFrameCount();
}

bool PlaybackSession::usesHardwareAcceleration() const noexcept
{
    return impl_->usesHardwareAcceleration();
}

std::string PlaybackSession::hardwareDeviceName() const
{
    return impl_->hardwareDeviceName();
}

} // namespace vidscope::playback
