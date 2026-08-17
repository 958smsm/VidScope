#pragma once

#include "media/MediaTypes.h"

#include <atomic>
#include <cstdint>

namespace vidscope::playback {

using RequestGeneration = std::uint64_t;

enum class SeekBias {
    AtOrAfter,
    AtOrBefore,
    Nearest,
};

struct SeekRequest final {
    RequestGeneration generation = 0;
    media::MediaTime target{};
    SeekBias bias = SeekBias::AtOrAfter;
};

struct SeekPlan final {
    std::int64_t targetStreamTimestamp = 0;
    std::int64_t minimumStreamTimestamp = INT64_MIN;
    std::int64_t maximumStreamTimestamp = 0;
    bool startsAtStreamOrigin = false;
};

class SeekController final {
public:
    [[nodiscard]] static SeekPlan plan(const media::MediaInfo& info, const SeekRequest& request) noexcept;
    [[nodiscard]] static bool frameSatisfies(
        const media::DecodedFrame& frame,
        const SeekRequest& request) noexcept;
};

class RequestGate final {
public:
    [[nodiscard]] RequestGeneration next() noexcept;
    [[nodiscard]] RequestGeneration current() const noexcept;
    [[nodiscard]] bool accepts(RequestGeneration generation) const noexcept;

private:
    std::atomic<RequestGeneration> current_{0};
};

} // namespace vidscope::playback
