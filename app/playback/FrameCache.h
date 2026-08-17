#pragma once

#include "media/MediaTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace vidscope::playback {

struct FrameCacheStats final {
    std::size_t frameCount = 0;
    std::size_t bytes = 0;
    std::size_t byteBudget = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
};

class FrameCache final {
public:
    explicit FrameCache(std::size_t byteBudget);
    ~FrameCache();
    FrameCache(const FrameCache&) = delete;
    FrameCache& operator=(const FrameCache&) = delete;

    bool insert(media::DecodedFramePtr frame);
    [[nodiscard]] media::DecodedFramePtr find(std::uint64_t sessionSerial);
    [[nodiscard]] media::DecodedFramePtr previous(const media::DecodedFrame& current);
    [[nodiscard]] media::DecodedFramePtr next(const media::DecodedFrame& current);
    [[nodiscard]] media::DecodedFramePtr previousKeyframe(const media::DecodedFrame& current);
    [[nodiscard]] std::vector<media::DecodedFramePtr> framesAfter(
        const media::DecodedFrame& current,
        std::size_t maximumCount);

    void pin(std::uint64_t sessionSerial);
    void clear();
    void setByteBudget(std::size_t bytes);
    [[nodiscard]] FrameCacheStats stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vidscope::playback
