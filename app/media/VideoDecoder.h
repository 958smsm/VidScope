#pragma once

#include "core/Cancellation.h"
#include "media/MediaSource.h"
#include "media/MediaTypes.h"

#include <cstdint>
#include <memory>
#include <string>

namespace vidscope::media {

enum class HardwareAcceleration {
    Auto,
    Disabled,
    D3D11VA,
    DXVA2,
    CUDA,
    VAAPI,
    VideoToolbox,
};

struct DecoderOptions final {
    HardwareAcceleration hardwareAcceleration = HardwareAcceleration::Auto;
    int threadCount = 0;
    bool allowSoftwareFallback = true;
};

enum class PacketSendStatus {
    Accepted,
    NeedReceive,
    EndOfStream,
    Cancelled,
};

enum class FrameReceiveStatus {
    Frame,
    NeedInput,
    EndOfStream,
    Cancelled,
};

struct FrameReceiveResult final {
    FrameReceiveStatus status = FrameReceiveStatus::NeedInput;
    DecodedFramePtr frame;
};

class VideoDecoder final {
public:
    static std::unique_ptr<VideoDecoder> create(
        const MediaSource& source,
        const DecoderOptions& options = {},
        core::CancellationToken cancellation = {});

    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;
    VideoDecoder(VideoDecoder&&) = delete;
    VideoDecoder& operator=(VideoDecoder&&) = delete;

    [[nodiscard]] PacketSendStatus sendPacket(
        const AVPacket* packet,
        core::CancellationToken cancellation = {});
    [[nodiscard]] FrameReceiveResult receiveFrame(
        std::uint64_t sessionSerial,
        std::int64_t presentationIndex,
        core::CancellationToken cancellation = {});

    void flush() noexcept;
    [[nodiscard]] bool usesHardwareAcceleration() const noexcept;
    [[nodiscard]] std::string hardwareDeviceName() const;
    [[nodiscard]] AVCodecContext* nativeHandle() noexcept;

private:
    class Impl;
    explicit VideoDecoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace vidscope::media
