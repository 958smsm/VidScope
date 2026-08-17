#include "media/Demuxer.h"

#include "core/Logging.h"
#include "media/FfmpegRaii.h"

#include <QtCore/QString>

#include <limits>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/error.h>
}

namespace vidscope::media {

Demuxer::Demuxer(MediaSource& source) noexcept
    : source_(source)
{
}

PacketReadStatus Demuxer::readNextVideoPacket(
    AVPacket* destination,
    core::CancellationToken cancellation)
{
    if (destination == nullptr) {
        throw std::invalid_argument("A destination packet is required");
    }

    source_.setCancellationToken(cancellation);
    av_packet_unref(destination);
    if (cancellation.isCancellationRequested()) {
        return PacketReadStatus::Cancelled;
    }

    AVFormatContext* format = source_.nativeHandle();
    if (format == nullptr || source_.videoStreamIndex() < 0) {
        throw std::logic_error("The demuxer has no open video source");
    }

    for (;;) {
        const int result = av_read_frame(format, destination);
        if (cancellation.isCancellationRequested()) {
            av_packet_unref(destination);
            return PacketReadStatus::Cancelled;
        }
        if (result == AVERROR_EOF) {
            av_packet_unref(destination);
            return PacketReadStatus::EndOfFile;
        }
        if (result < 0) {
            av_packet_unref(destination);
            if (result == AVERROR_EXIT) {
                return PacketReadStatus::Cancelled;
            }
            throw FfmpegError("Read the next media packet", result);
        }

        if (destination->stream_index == source_.videoStreamIndex()) {
            return PacketReadStatus::Packet;
        }
        av_packet_unref(destination);
    }
}

bool Demuxer::seekToTimestamp(
    const std::int64_t absoluteStreamTimestamp,
    core::CancellationToken cancellation)
{
    source_.setCancellationToken(cancellation);
    if (cancellation.isCancellationRequested()) {
        return false;
    }

    AVFormatContext* format = source_.nativeHandle();
    const int streamIndex = source_.videoStreamIndex();
    if (format == nullptr || streamIndex < 0) {
        return false;
    }

    int result = avformat_seek_file(
        format,
        streamIndex,
        std::numeric_limits<std::int64_t>::min(),
        absoluteStreamTimestamp,
        absoluteStreamTimestamp,
        AVSEEK_FLAG_BACKWARD);
    if (result < 0 && !cancellation.isCancellationRequested()) {
        // A few demuxers implement only the older seek entry point.
        result = av_seek_frame(
            format, streamIndex, absoluteStreamTimestamp, AVSEEK_FLAG_BACKWARD);
    }

    if (cancellation.isCancellationRequested() || result == AVERROR_EXIT) {
        return false;
    }
    if (result < 0) {
        qCWarning(logSeek).noquote()
            << "Could not seek video stream to" << absoluteStreamTimestamp << ':'
            << QString::fromStdString(ffmpegErrorString(result));
        return false;
    }

    qCDebug(logSeek) << "Seeked stream" << streamIndex << "to/before"
                     << absoluteStreamTimestamp;
    return true;
}

bool Demuxer::seekToStart(core::CancellationToken cancellation)
{
    return seekToTimestamp(source_.info().streamStartTimestamp, std::move(cancellation));
}

} // namespace vidscope::media
