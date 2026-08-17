#pragma once

#include <memory>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

namespace vidscope::media {

[[nodiscard]] std::string ffmpegErrorString(int errorCode);
void ensureFfmpegNetworkInitialized();

class FfmpegError : public std::runtime_error {
public:
    FfmpegError(std::string operation, int errorCode);
    [[nodiscard]] int errorCode() const noexcept { return errorCode_; }

private:
    int errorCode_;
};

struct FormatContextDeleter {
    void operator()(AVFormatContext* context) const noexcept;
};
struct CodecContextDeleter {
    void operator()(AVCodecContext* context) const noexcept;
};
struct PacketDeleter {
    void operator()(AVPacket* packet) const noexcept;
};
struct FrameDeleter {
    void operator()(AVFrame* frame) const noexcept;
};
struct SwsContextDeleter {
    void operator()(SwsContext* context) const noexcept;
};
struct BufferRefDeleter {
    void operator()(AVBufferRef* buffer) const noexcept;
};

using FormatContextPtr = std::unique_ptr<AVFormatContext, FormatContextDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using SwsContextPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;
using BufferRefPtr = std::unique_ptr<AVBufferRef, BufferRefDeleter>;

[[nodiscard]] PacketPtr makePacket();
[[nodiscard]] FramePtr makeFrame();

} // namespace vidscope::media
