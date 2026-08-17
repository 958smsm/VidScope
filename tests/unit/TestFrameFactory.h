#pragma once

#include "media/FfmpegRaii.h"
#include "media/MediaTypes.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace vidscope::test {

inline media::DecodedFramePtr makeTestFrame(
    std::uint64_t serial,
    std::chrono::nanoseconds time,
    bool keyFrame = false,
    int width = 64,
    int height = 64)
{
    auto source = media::makeFrame();
    source->format = AV_PIX_FMT_GRAY8;
    source->width = width;
    source->height = height;
    const int result = av_frame_get_buffer(source.get(), 32);
    if (result < 0) {
        throw media::FfmpegError("allocate test frame", result);
    }

    auto frame = std::make_shared<media::DecodedFrame>();
    frame->storage = std::make_shared<media::FrameStorage>(source.get());
    frame->id.sessionSerial = serial;
    frame->id.presentationIndex = static_cast<std::int64_t>(serial);
    frame->id.pts = static_cast<std::int64_t>(time.count());
    frame->bestEffortTimestamp = frame->id.pts;
    frame->timeBase = AVRational{1, 1'000'000'000};
    frame->presentationTime = time;
    frame->duration = std::chrono::milliseconds(40);
    frame->keyFrame = keyFrame;
    frame->width = width;
    frame->height = height;
    frame->pixelFormat = AV_PIX_FMT_GRAY8;
    return frame;
}

} // namespace vidscope::test
