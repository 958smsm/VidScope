#include "media/MediaSource.h"

#include "core/Logging.h"

#include <QtCore/QString>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/codec_desc.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
}

namespace vidscope::media {
namespace {

std::string pathToUtf8(const std::filesystem::path& path)
{
#if defined(_WIN32)
    const std::u8string value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
#else
    return path.string();
#endif
}

int pixelFormatBitDepth(const AVPixelFormat format) noexcept
{
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(format);
    if (descriptor == nullptr) {
        return 0;
    }

    int depth = 0;
    for (int component = 0; component < descriptor->nb_components; ++component) {
        depth = std::max(depth, static_cast<int>(descriptor->comp[component].depth));
    }
    return depth;
}

MediaTime determineDuration(const AVFormatContext* format, const AVStream* stream) noexcept
{
    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0
        && stream->time_base.num > 0 && stream->time_base.den > 0) {
        return timestampToMediaTime(stream->duration, 0, stream->time_base);
    }
    if (format->duration != AV_NOPTS_VALUE && format->duration > 0) {
        auto normalizedDuration = format->duration;
        // Some demuxers report an absolute end timestamp when input timestamps
        // begin after zero. Convert that extent to application-relative duration.
        auto normalizedOrigin = format->start_time;
        if (stream->start_time != AV_NOPTS_VALUE && stream->time_base.num > 0
            && stream->time_base.den > 0) {
            normalizedOrigin =
                av_rescale_q(stream->start_time, stream->time_base, AV_TIME_BASE_Q);
        }
        if (normalizedOrigin != AV_NOPTS_VALUE && normalizedOrigin > 0
            && normalizedDuration > normalizedOrigin) {
            normalizedDuration -= normalizedOrigin;
        }
        return timestampToMediaTime(normalizedDuration, 0, AV_TIME_BASE_Q);
    }
    return MediaTime::zero();
}

std::int64_t determineStreamOrigin(
    const AVFormatContext* format,
    const AVStream* stream) noexcept
{
    if (stream->start_time != AV_NOPTS_VALUE) {
        return stream->start_time;
    }
    if (format->start_time != AV_NOPTS_VALUE && stream->time_base.num > 0
        && stream->time_base.den > 0) {
        return av_rescale_q(format->start_time, AV_TIME_BASE_Q, stream->time_base);
    }
    return 0;
}

MediaInfo buildMediaInfo(
    const std::filesystem::path& path,
    const AVFormatContext* format,
    const int videoStreamIndex)
{
    const AVStream* stream = format->streams[videoStreamIndex];
    const AVCodecParameters* parameters = stream->codecpar;
    const AVCodecDescriptor* descriptor = avcodec_descriptor_get(parameters->codec_id);

    MediaInfo info;
    info.path = path;
    if (format->iformat != nullptr) {
        info.containerName = format->iformat->name != nullptr ? format->iformat->name : "";
        info.containerLongName =
            format->iformat->long_name != nullptr ? format->iformat->long_name : "";
    }
    info.codecName = descriptor != nullptr && descriptor->name != nullptr
        ? descriptor->name
        : avcodec_get_name(parameters->codec_id);
    info.codecLongName = descriptor != nullptr && descriptor->long_name != nullptr
        ? descriptor->long_name
        : info.codecName;
    info.videoStreamIndex = videoStreamIndex;
    info.width = parameters->width;
    info.height = parameters->height;
    info.pixelFormat = static_cast<AVPixelFormat>(parameters->format);
    info.timeBase = stream->time_base;
    info.averageFrameRate = stream->avg_frame_rate;
    info.realFrameRate = stream->r_frame_rate;
    info.streamStartTimestamp = determineStreamOrigin(format, stream);
    info.duration = determineDuration(format, stream);
    info.declaredFrameCount = stream->nb_frames > 0 ? stream->nb_frames : 0;
    info.codecDelayFrames = std::max(parameters->video_delay, 0);
    info.bitDepth = parameters->bits_per_raw_sample > 0
        ? parameters->bits_per_raw_sample
        : pixelFormatBitDepth(info.pixelFormat);
    info.colorRange = parameters->color_range;
    info.colorSpace = parameters->color_space;
    info.colorPrimaries = parameters->color_primaries;
    info.colorTransfer = parameters->color_trc;
    return info;
}

} // namespace

struct MediaSource::InterruptState final {
    explicit InterruptState(core::CancellationToken initialToken)
        : token(std::move(initialToken))
    {
    }

    static int callback(void* opaque) noexcept
    {
        auto* state = static_cast<InterruptState*>(opaque);
        if (state == nullptr) {
            return 0;
        }

        core::CancellationToken snapshot;
        {
            std::lock_guard lock(state->mutex);
            snapshot = state->token;
        }
        return snapshot.isCancellationRequested() ? 1 : 0;
    }

    void setToken(core::CancellationToken replacement)
    {
        std::lock_guard lock(mutex);
        token = std::move(replacement);
    }

    std::mutex mutex;
    core::CancellationToken token;
};

std::unique_ptr<MediaSource> MediaSource::open(
    const std::filesystem::path& path,
    const MediaOpenOptions& options,
    core::CancellationToken cancellation)
{
    if (path.empty()) {
        throw std::invalid_argument("Cannot open an empty media path");
    }
    if (cancellation.isCancellationRequested()) {
        throw FfmpegError("Open media cancelled", AVERROR_EXIT);
    }

    static std::once_flag networkInitialization;
    std::call_once(networkInitialization, [] { avformat_network_init(); });

    auto interrupt = std::make_unique<InterruptState>(std::move(cancellation));
    AVFormatContext* rawFormat = avformat_alloc_context();
    if (rawFormat == nullptr) {
        throw std::bad_alloc();
    }
    rawFormat->interrupt_callback.callback = &InterruptState::callback;
    rawFormat->interrupt_callback.opaque = interrupt.get();
    if (options.generateMissingPresentationTimestamps) {
        rawFormat->flags |= AVFMT_FLAG_GENPTS;
    }

    const std::string inputPath = pathToUtf8(path);
    const int openResult = avformat_open_input(&rawFormat, inputPath.c_str(), nullptr, nullptr);
    FormatContextPtr format(rawFormat);
    if (openResult < 0) {
        throw FfmpegError("Open media '" + inputPath + "'", openResult);
    }
    if (InterruptState::callback(interrupt.get()) != 0) {
        throw FfmpegError("Open media cancelled", AVERROR_EXIT);
    }

    const int streamInfoResult = avformat_find_stream_info(format.get(), nullptr);
    if (streamInfoResult < 0) {
        if (InterruptState::callback(interrupt.get()) != 0) {
            throw FfmpegError("Read media metadata cancelled", AVERROR_EXIT);
        }
        throw FfmpegError("Read media stream information", streamInfoResult);
    }

    int videoStreamIndex = -1;
    if (options.preferredVideoStream.has_value()) {
        const int requested = *options.preferredVideoStream;
        if (requested < 0 || static_cast<unsigned int>(requested) >= format->nb_streams
            || format->streams[requested] == nullptr
            || format->streams[requested]->codecpar == nullptr
            || format->streams[requested]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
            throw std::invalid_argument("The preferred stream is not a valid video stream");
        }
        videoStreamIndex = requested;
    } else {
        videoStreamIndex =
            av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (videoStreamIndex < 0) {
            throw FfmpegError("Find a usable video stream", videoStreamIndex);
        }
    }

    MediaInfo info = buildMediaInfo(path, format.get(), videoStreamIndex);
    qCInfo(logDemux).noquote()
        << "Opened" << QString::fromStdString(inputPath)
        << "stream" << videoStreamIndex
        << QString::fromStdString(info.codecName)
        << info.width << 'x' << info.height;

    return std::unique_ptr<MediaSource>(
        new MediaSource(std::move(format), std::move(info), std::move(interrupt)));
}

MediaSource::MediaSource(
    FormatContextPtr format,
    MediaInfo info,
    std::unique_ptr<InterruptState> interrupt)
    : format_(std::move(format))
    , info_(std::move(info))
    , interrupt_(std::move(interrupt))
{
}

MediaSource::~MediaSource()
{
    // AVFormatContext retains interrupt_.get() as its opaque callback pointer.
    format_.reset();
    interrupt_.reset();
}

AVStream* MediaSource::videoStream() noexcept
{
    if (!format_ || info_.videoStreamIndex < 0
        || static_cast<unsigned int>(info_.videoStreamIndex) >= format_->nb_streams) {
        return nullptr;
    }
    return format_->streams[info_.videoStreamIndex];
}

const AVStream* MediaSource::videoStream() const noexcept
{
    if (!format_ || info_.videoStreamIndex < 0
        || static_cast<unsigned int>(info_.videoStreamIndex) >= format_->nb_streams) {
        return nullptr;
    }
    return format_->streams[info_.videoStreamIndex];
}

void MediaSource::setCancellationToken(core::CancellationToken cancellation)
{
    if (interrupt_) {
        interrupt_->setToken(std::move(cancellation));
    }
}

} // namespace vidscope::media


