#include "playback/SeekController.h"

#include <algorithm>
#include <limits>

namespace vidscope::playback {

SeekPlan SeekController::plan(
    const media::MediaInfo& info,
    const SeekRequest& request) noexcept
{
    auto target = std::max(request.target, media::MediaTime::zero());
    if (info.duration > media::MediaTime::zero()) {
        target = std::min(target, info.duration);
    }

    auto origin = info.streamStartTimestamp;
    if (origin == AV_NOPTS_VALUE) {
        origin = 0;
    }
    auto timestamp = media::mediaTimeToTimestamp(target, origin, info.timeBase);
    if (timestamp == AV_NOPTS_VALUE) {
        timestamp = origin;
    }

    if (request.bias != SeekBias::AtOrAfter && timestamp > origin) {
        const auto roundedTime =
            media::timestampToMediaTime(timestamp, origin, info.timeBase);
        if (roundedTime != media::kNoMediaTime && roundedTime > target) {
            --timestamp;
        }
    }

    SeekPlan result;
    result.targetStreamTimestamp = timestamp;
    result.minimumStreamTimestamp = std::numeric_limits<std::int64_t>::min();
    result.maximumStreamTimestamp = timestamp;
    result.startsAtStreamOrigin = target == media::MediaTime::zero();
    return result;
}

bool SeekController::frameSatisfies(
    const media::DecodedFrame& frame,
    const SeekRequest& request) noexcept
{
    if (frame.presentationTime == media::kNoMediaTime) {
        return false;
    }

    switch (request.bias) {
    case SeekBias::AtOrAfter:
        return frame.presentationTime >= request.target;
    case SeekBias::AtOrBefore:
        return frame.presentationTime <= request.target;
    case SeekBias::Nearest:
        // Nearest selection requires a bracket. An exact frame independently
        // satisfies the request; PlaybackSession compares the two neighbours.
        return frame.presentationTime == request.target;
    }
    return false;
}

RequestGeneration RequestGate::next() noexcept
{
    auto observed = current_.load(std::memory_order_relaxed);
    for (;;) {
        const auto desired = observed == std::numeric_limits<RequestGeneration>::max()
            ? RequestGeneration{1}
            : observed + 1;
        if (current_.compare_exchange_weak(
                observed,
                desired,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return desired;
        }
    }
}

RequestGeneration RequestGate::current() const noexcept
{
    return current_.load(std::memory_order_acquire);
}

bool RequestGate::accepts(const RequestGeneration generation) const noexcept
{
    return current() == generation;
}

} // namespace vidscope::playback


