#pragma once

#include "core/Cancellation.h"
#include "media/MediaTypes.h"
#include "media/VideoDecoder.h"
#include "playback/FrameCache.h"
#include "playback/SeekController.h"

#include <cstddef>
#include <filesystem>
#include <memory>

namespace vidscope::playback {

struct PlaybackSessionConfig final {
    std::size_t frameCacheBytes = 512ULL * 1024ULL * 1024ULL;
    std::size_t forwardQueueBytes = 192ULL * 1024ULL * 1024ULL;
    std::size_t forwardQueueFrames = 16;
    std::size_t initialPrefetchFrames = 8;
    std::size_t presentationIndexAnchorCount = 65'536;
    media::DecoderOptions decoder;
};

enum class NavigationStatus {
    FrameReady,
    BeginningOfStream,
    EndOfStream,
    Cancelled,
    NoMedia,
};

struct NavigationResult final {
    NavigationStatus status = NavigationStatus::NoMedia;
    media::DecodedFramePtr frame;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return status == NavigationStatus::FrameReady && static_cast<bool>(frame);
    }
};

// Thread-confined synchronous engine. PlaybackController is its async Qt adapter.
class PlaybackSession final {
public:
    explicit PlaybackSession(PlaybackSessionConfig config = {});
    ~PlaybackSession();
    PlaybackSession(const PlaybackSession&) = delete;
    PlaybackSession& operator=(const PlaybackSession&) = delete;

    [[nodiscard]] NavigationResult open(
        const std::filesystem::path& path,
        core::CancellationToken cancellation = {});
    void close() noexcept;

    [[nodiscard]] NavigationResult seek(
        const SeekRequest& request,
        core::CancellationToken cancellation = {});
    [[nodiscard]] NavigationResult nextFrame(core::CancellationToken cancellation = {});
    [[nodiscard]] NavigationResult previousFrame(core::CancellationToken cancellation = {});
    [[nodiscard]] NavigationResult nextKeyframe(core::CancellationToken cancellation = {});
    [[nodiscard]] NavigationResult previousKeyframe(core::CancellationToken cancellation = {});
    void prefetch(core::CancellationToken cancellation = {});

    [[nodiscard]] bool isOpen() const noexcept;
    [[nodiscard]] const media::MediaInfo* mediaInfo() const noexcept;
    [[nodiscard]] media::DecodedFramePtr currentFrame() const noexcept;
    [[nodiscard]] FrameCacheStats cacheStats() const;
    [[nodiscard]] bool usesHardwareAcceleration() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vidscope::playback


