#include "media/FfmpegRaii.h"

#include <array>
#include <new>
#include <utility>

extern "C" {
#include <libavutil/error.h>
}

namespace vidscope::media {
namespace {

std::string makeErrorMessage(std::string operation, const int errorCode)
{
    std::string message = std::move(operation);
    if (!message.empty()) {
        message += ": ";
    }
    message += ffmpegErrorString(errorCode);
    message += " (FFmpeg error ";
    message += std::to_string(errorCode);
    message += ')';
    return message;
}

} // namespace

std::string ffmpegErrorString(const int errorCode)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    if (av_strerror(errorCode, buffer.data(), buffer.size()) < 0) {
        return "Unknown FFmpeg error";
    }
    return buffer.data();
}

FfmpegError::FfmpegError(std::string operation, const int errorCode)
    : std::runtime_error(makeErrorMessage(std::move(operation), errorCode))
    , errorCode_(errorCode)
{
}

void FormatContextDeleter::operator()(AVFormatContext* context) const noexcept
{
    if (context != nullptr) {
        avformat_close_input(&context);
    }
}

void CodecContextDeleter::operator()(AVCodecContext* context) const noexcept
{
    if (context != nullptr) {
        avcodec_free_context(&context);
    }
}

void PacketDeleter::operator()(AVPacket* packet) const noexcept
{
    if (packet != nullptr) {
        av_packet_free(&packet);
    }
}

void FrameDeleter::operator()(AVFrame* frame) const noexcept
{
    if (frame != nullptr) {
        av_frame_free(&frame);
    }
}

void SwsContextDeleter::operator()(SwsContext* context) const noexcept
{
    sws_freeContext(context);
}

void BufferRefDeleter::operator()(AVBufferRef* buffer) const noexcept
{
    if (buffer != nullptr) {
        av_buffer_unref(&buffer);
    }
}

PacketPtr makePacket()
{
    PacketPtr packet(av_packet_alloc());
    if (!packet) {
        throw std::bad_alloc();
    }
    return packet;
}

FramePtr makeFrame()
{
    FramePtr frame(av_frame_alloc());
    if (!frame) {
        throw std::bad_alloc();
    }
    return frame;
}

} // namespace vidscope::media
