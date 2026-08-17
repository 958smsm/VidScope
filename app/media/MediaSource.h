#pragma once

#include "core/Cancellation.h"
#include "media/FfmpegRaii.h"
#include "media/MediaTypes.h"

#include <filesystem>
#include <memory>
#include <optional>

namespace vidscope::media {

struct MediaOpenOptions final {
    std::optional<int> preferredVideoStream;
    bool generateMissingPresentationTimestamps = true;
};

class MediaSource final {
public:
    static std::unique_ptr<MediaSource> open(
        const std::filesystem::path& path,
        const MediaOpenOptions& options = {},
        core::CancellationToken cancellation = {});

    ~MediaSource();
    MediaSource(const MediaSource&) = delete;
    MediaSource& operator=(const MediaSource&) = delete;
    MediaSource(MediaSource&&) = delete;
    MediaSource& operator=(MediaSource&&) = delete;

    [[nodiscard]] AVFormatContext* nativeHandle() noexcept { return format_.get(); }
    [[nodiscard]] const AVFormatContext* nativeHandle() const noexcept { return format_.get(); }
    [[nodiscard]] AVStream* videoStream() noexcept;
    [[nodiscard]] const AVStream* videoStream() const noexcept;
    [[nodiscard]] int videoStreamIndex() const noexcept { return info_.videoStreamIndex; }
    [[nodiscard]] const MediaInfo& info() const noexcept { return info_; }

    void setCancellationToken(core::CancellationToken cancellation);

private:
    struct InterruptState;
    MediaSource(FormatContextPtr format, MediaInfo info, std::unique_ptr<InterruptState> interrupt);

    FormatContextPtr format_;
    MediaInfo info_;
    std::unique_ptr<InterruptState> interrupt_;
};

} // namespace vidscope::media
