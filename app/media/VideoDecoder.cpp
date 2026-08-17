#include "media/VideoDecoder.h"

#include "core/Logging.h"
#include "media/FfmpegRaii.h"

#include <QtCore/QString>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
#include <libavutil/version.h>
}

namespace vidscope::media {
namespace {

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

bool isHardwarePixelFormat(const AVPixelFormat format) noexcept
{
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(format);
    return descriptor != nullptr && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0;
}

std::vector<AVHWDeviceType> requestedDeviceTypes(const HardwareAcceleration requested)
{
    switch (requested) {
    case HardwareAcceleration::Disabled:
        return {};
    case HardwareAcceleration::D3D11VA:
        return {AV_HWDEVICE_TYPE_D3D11VA};
    case HardwareAcceleration::DXVA2:
        return {AV_HWDEVICE_TYPE_DXVA2};
    case HardwareAcceleration::CUDA:
        return {AV_HWDEVICE_TYPE_CUDA};
    case HardwareAcceleration::VAAPI:
        return {AV_HWDEVICE_TYPE_VAAPI};
    case HardwareAcceleration::VideoToolbox:
        return {AV_HWDEVICE_TYPE_VIDEOTOOLBOX};
    case HardwareAcceleration::Auto:
#if defined(_WIN32)
        // D3D11VA is the preferred Windows path; DXVA2 remains a compatibility fallback.
        return {AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2};
#elif defined(__APPLE__)
        return {AV_HWDEVICE_TYPE_VIDEOTOOLBOX};
#else
        return {AV_HWDEVICE_TYPE_VAAPI};
#endif
    }
    return {};
}

template<typename T>
T metadataOrFallback(const T value, const T unspecified, const T fallback) noexcept
{
    return value != unspecified ? value : fallback;
}

} // namespace

class VideoDecoder::Impl final {
public:
    Impl(
        const MediaSource& source,
        const DecoderOptions& options,
        const core::CancellationToken& cancellation)
        : info_(source.info())
        , allowSoftwareFallback_(options.allowSoftwareFallback)
        , receiveFrame_(makeFrame())
        , transferFrame_(makeFrame())
    {
        if (options.threadCount < 0) {
            throw std::invalid_argument("Decoder thread count cannot be negative");
        }
        if (cancellation.isCancellationRequested()) {
            throw FfmpegError("Create video decoder cancelled", AVERROR_EXIT);
        }

        const AVStream* stream = source.videoStream();
        if (stream == nullptr || stream->codecpar == nullptr) {
            throw std::invalid_argument("The media source has no selected video stream");
        }

        decoder_ = avcodec_find_decoder(stream->codecpar->codec_id);
        if (decoder_ == nullptr) {
            throw FfmpegError("Find a video decoder", AVERROR_DECODER_NOT_FOUND);
        }

        int lastHardwareError = AVERROR(ENOSYS);
        bool attemptedHardware = false;
        for (const AVHWDeviceType type : requestedDeviceTypes(options.hardwareAcceleration)) {
            if (cancellation.isCancellationRequested()) {
                throw FfmpegError("Create video decoder cancelled", AVERROR_EXIT);
            }
            attemptedHardware = true;
            if (tryOpenHardware(stream, options, type, lastHardwareError)) {
                return;
            }
        }

        if (attemptedHardware && !options.allowSoftwareFallback) {
            throw FfmpegError("Initialize the requested hardware video decoder", lastHardwareError);
        }
        if (attemptedHardware) {
            qCWarning(logGpu).noquote()
                << "Hardware decoding unavailable; using the CPU decoder:"
                << QString::fromStdString(ffmpegErrorString(lastHardwareError));
        }

        openSoftware(stream, options);
    }

    PacketSendStatus sendPacket(
        const AVPacket* packet,
        const core::CancellationToken& cancellation)
    {
        if (cancellation.isCancellationRequested()) {
            return PacketSendStatus::Cancelled;
        }
        if (decoderEof_) {
            return PacketSendStatus::EndOfStream;
        }
        if (drainSent_) {
            return packet == nullptr ? PacketSendStatus::Accepted
                                     : PacketSendStatus::EndOfStream;
        }

        const int result = avcodec_send_packet(codec_.get(), packet);
        if (cancellation.isCancellationRequested()) {
            return PacketSendStatus::Cancelled;
        }
        if (result == 0) {
            if (packet == nullptr) {
                drainSent_ = true;
            }
            return PacketSendStatus::Accepted;
        }
        if (result == AVERROR(EAGAIN)) {
            return PacketSendStatus::NeedReceive;
        }
        if (result == AVERROR_EOF) {
            decoderEof_ = true;
            drainSent_ = packet == nullptr;
            return PacketSendStatus::EndOfStream;
        }
        if (result == AVERROR_EXIT) {
            return PacketSendStatus::Cancelled;
        }
        throw FfmpegError(packet != nullptr ? "Send a video packet" : "Drain the video decoder", result);
    }

    FrameReceiveResult receive(
        const std::uint64_t sessionSerial,
        const std::int64_t presentationIndex,
        const core::CancellationToken& cancellation)
    {
        if (cancellation.isCancellationRequested()) {
            return {FrameReceiveStatus::Cancelled, {}};
        }
        if (decoderEof_) {
            return {FrameReceiveStatus::EndOfStream, {}};
        }

        av_frame_unref(receiveFrame_.get());
        const int result = avcodec_receive_frame(codec_.get(), receiveFrame_.get());
        if (cancellation.isCancellationRequested()) {
            av_frame_unref(receiveFrame_.get());
            return {FrameReceiveStatus::Cancelled, {}};
        }
        if (result == AVERROR(EAGAIN)) {
            return {FrameReceiveStatus::NeedInput, {}};
        }
        if (result == AVERROR_EOF) {
            decoderEof_ = true;
            return {FrameReceiveStatus::EndOfStream, {}};
        }
        if (result == AVERROR_EXIT) {
            return {FrameReceiveStatus::Cancelled, {}};
        }
        if (result < 0) {
            throw FfmpegError("Receive a decoded video frame", result);
        }

        const AVFrame* publishableFrame = receiveFrame_.get();
        if (receiveFrame_->hw_frames_ctx != nullptr
            || isHardwarePixelFormat(static_cast<AVPixelFormat>(receiveFrame_->format))) {
            av_frame_unref(transferFrame_.get());
            const int transferResult =
                av_hwframe_transfer_data(transferFrame_.get(), receiveFrame_.get(), 0);
            if (transferResult < 0) {
                throw FfmpegError("Transfer a hardware video frame to system memory", transferResult);
            }
            const int propertiesResult =
                av_frame_copy_props(transferFrame_.get(), receiveFrame_.get());
            if (propertiesResult < 0) {
                throw FfmpegError("Copy hardware video frame metadata", propertiesResult);
            }
            publishableFrame = transferFrame_.get();
            hardwareSelected_.store(true, std::memory_order_release);
        }

        if (cancellation.isCancellationRequested()) {
            return {FrameReceiveStatus::Cancelled, {}};
        }
        return {
            FrameReceiveStatus::Frame,
            makeDecodedFrame(*publishableFrame, sessionSerial, presentationIndex),
        };
    }

    void flush() noexcept
    {
        if (codec_) {
            avcodec_flush_buffers(codec_.get());
        }
        av_frame_unref(receiveFrame_.get());
        av_frame_unref(transferFrame_.get());
        drainSent_ = false;
        decoderEof_ = false;
    }

    bool usesHardwareAcceleration() const noexcept
    {
        return hardwareConfigured_ && hardwareSelected_.load(std::memory_order_acquire);
    }

    std::string hardwareDeviceName() const
    {
        return usesHardwareAcceleration() ? hardwareDeviceName_ : std::string{};
    }

    AVCodecContext* nativeHandle() noexcept
    {
        return codec_.get();
    }

private:
    static AVPixelFormat selectPixelFormat(
        AVCodecContext* codecContext,
        const AVPixelFormat* candidates)
    {
        auto* self = static_cast<Impl*>(codecContext->opaque);
        if (self == nullptr || candidates == nullptr) {
            return AV_PIX_FMT_NONE;
        }

        for (const AVPixelFormat* candidate = candidates; *candidate != AV_PIX_FMT_NONE;
             ++candidate) {
            if (*candidate == self->hardwarePixelFormat_) {
                self->hardwareSelected_.store(true, std::memory_order_release);
                return *candidate;
            }
        }

        self->hardwareSelected_.store(false, std::memory_order_release);
        if (!self->allowSoftwareFallback_) {
            return AV_PIX_FMT_NONE;
        }
        qCWarning(logGpu) << "Decoder did not offer the configured hardware pixel format;"
                             " choosing a software format";
        for (const AVPixelFormat* candidate = candidates; *candidate != AV_PIX_FMT_NONE;
             ++candidate) {
            if (!isHardwarePixelFormat(*candidate)) {
                return *candidate;
            }
        }
        return avcodec_default_get_format(codecContext, candidates);
    }

    CodecContextPtr makeContext(const AVStream* stream, const DecoderOptions& options) const
    {
        CodecContextPtr context(avcodec_alloc_context3(decoder_));
        if (!context) {
            throw std::bad_alloc();
        }

        const int parametersResult =
            avcodec_parameters_to_context(context.get(), stream->codecpar);
        if (parametersResult < 0) {
            throw FfmpegError("Copy video decoder parameters", parametersResult);
        }
        context->pkt_timebase = stream->time_base;
        if (options.threadCount > 0) {
            context->thread_count = options.threadCount;
        }
        return context;
    }

    bool tryOpenHardware(
        const AVStream* stream,
        const DecoderOptions& options,
        const AVHWDeviceType deviceType,
        int& error)
    {
        const AVCodecHWConfig* hardwareConfig = nullptr;
        for (int index = 0;; ++index) {
            const AVCodecHWConfig* candidate = avcodec_get_hw_config(decoder_, index);
            if (candidate == nullptr) {
                break;
            }
            if (candidate->device_type == deviceType
                && (candidate->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
                hardwareConfig = candidate;
                break;
            }
        }
        if (hardwareConfig == nullptr) {
            error = AVERROR(ENOSYS);
            return false;
        }

        AVBufferRef* rawDevice = nullptr;
        error = av_hwdevice_ctx_create(&rawDevice, deviceType, nullptr, nullptr, 0);
        BufferRefPtr device(rawDevice);
        if (error < 0) {
            return false;
        }

        CodecContextPtr context = makeContext(stream, options);
        hardwarePixelFormat_ = hardwareConfig->pix_fmt;
        hardwareSelected_.store(true, std::memory_order_release);
        context->opaque = this;
        context->get_format = &Impl::selectPixelFormat;
        context->hw_device_ctx = av_buffer_ref(device.get());
        if (context->hw_device_ctx == nullptr) {
            error = AVERROR(ENOMEM);
            hardwarePixelFormat_ = AV_PIX_FMT_NONE;
            hardwareSelected_.store(false, std::memory_order_release);
            return false;
        }

        error = avcodec_open2(context.get(), decoder_, nullptr);
        if (error < 0) {
            hardwarePixelFormat_ = AV_PIX_FMT_NONE;
            hardwareSelected_.store(false, std::memory_order_release);
            return false;
        }

        codec_ = std::move(context);
        hardwareDevice_ = std::move(device);
        hardwareConfigured_ = true;
        const char* name = av_hwdevice_get_type_name(deviceType);
        hardwareDeviceName_ = name != nullptr ? name : "hardware";
        qCInfo(logGpu) << "Configured" << QString::fromStdString(hardwareDeviceName_)
                       << "video decoding for" << decoder_->name;
        return true;
    }

    void openSoftware(const AVStream* stream, const DecoderOptions& options)
    {
        CodecContextPtr context = makeContext(stream, options);
        const int result = avcodec_open2(context.get(), decoder_, nullptr);
        if (result < 0) {
            throw FfmpegError("Open the software video decoder", result);
        }

        codec_ = std::move(context);
        hardwareDevice_.reset();
        hardwarePixelFormat_ = AV_PIX_FMT_NONE;
        hardwareConfigured_ = false;
        hardwareSelected_.store(false, std::memory_order_release);
        hardwareDeviceName_.clear();
        qCInfo(logDecoder) << "Opened software video decoder" << decoder_->name;
    }

    DecodedFramePtr makeDecodedFrame(
        const AVFrame& source,
        const std::uint64_t sessionSerial,
        const std::int64_t presentationIndex) const
    {
        auto decoded = std::make_shared<DecodedFrame>();
        decoded->storage = std::make_shared<FrameStorage>(&source);
        decoded->id.presentationIndex = presentationIndex;
        decoded->id.pts = source.pts != AV_NOPTS_VALUE
            ? source.pts
            : source.best_effort_timestamp;
        decoded->id.sessionSerial = sessionSerial;
        decoded->dts = source.pkt_dts;
        decoded->bestEffortTimestamp = source.best_effort_timestamp;
        decoded->timeBase = info_.timeBase;

        std::int64_t presentationTimestamp = source.best_effort_timestamp;
        if (presentationTimestamp == AV_NOPTS_VALUE) {
            presentationTimestamp = source.pts;
        }
        if (presentationTimestamp == AV_NOPTS_VALUE) {
            presentationTimestamp = source.pkt_dts;
        }
        decoded->presentationTime = timestampToMediaTime(
            presentationTimestamp, info_.streamStartTimestamp, info_.timeBase);

#if LIBAVUTIL_VERSION_MAJOR >= 58
        const std::int64_t durationTimestamp = source.duration;
#else
        const std::int64_t durationTimestamp = source.pkt_duration;
#endif
        // Nominal duration is only a playback scheduling fallback, not frame metadata.
        decoded->duration = durationTimestamp > 0
            ? timestampToMediaTime(durationTimestamp, 0, info_.timeBase)
            : MediaTime::zero();
        decoded->keyFrame = (source.flags & AV_FRAME_FLAG_KEY) != 0;
        decoded->pictureType = source.pict_type;
        decoded->codedPictureNumber = -1;
        decoded->displayPictureNumber = -1;
        decoded->width = source.width;
        decoded->height = source.height;
        decoded->pixelFormat = static_cast<AVPixelFormat>(source.format);
        decoded->colorRange = metadataOrFallback(
            source.color_range, AVCOL_RANGE_UNSPECIFIED, info_.colorRange);
        decoded->colorSpace = metadataOrFallback(
            source.colorspace, AVCOL_SPC_UNSPECIFIED, info_.colorSpace);
        decoded->colorPrimaries = metadataOrFallback(
            source.color_primaries, AVCOL_PRI_UNSPECIFIED, info_.colorPrimaries);
        decoded->colorTransfer = metadataOrFallback(
            source.color_trc, AVCOL_TRC_UNSPECIFIED, info_.colorTransfer);
        decoded->chromaLocation = source.chroma_location;
        decoded->bitDepth = pixelFormatBitDepth(decoded->pixelFormat);
        if (decoded->bitDepth <= 0) {
            decoded->bitDepth = info_.bitDepth;
        }
        decoded->masteringDisplay = extractMasteringDisplayMetadata(source);
        decoded->contentLight = extractContentLightMetadata(source);
        return decoded;
    }

    MediaInfo info_;
    bool allowSoftwareFallback_ = true;
    const AVCodec* decoder_ = nullptr;
    CodecContextPtr codec_;
    BufferRefPtr hardwareDevice_;
    FramePtr receiveFrame_;
    FramePtr transferFrame_;
    AVPixelFormat hardwarePixelFormat_ = AV_PIX_FMT_NONE;
    bool hardwareConfigured_ = false;
    std::atomic_bool hardwareSelected_{false};
    std::string hardwareDeviceName_;
    bool drainSent_ = false;
    bool decoderEof_ = false;
};

std::unique_ptr<VideoDecoder> VideoDecoder::create(
    const MediaSource& source,
    const DecoderOptions& options,
    core::CancellationToken cancellation)
{
    return std::unique_ptr<VideoDecoder>(
        new VideoDecoder(std::make_unique<Impl>(source, options, cancellation)));
}

VideoDecoder::VideoDecoder(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

VideoDecoder::~VideoDecoder() = default;

PacketSendStatus VideoDecoder::sendPacket(
    const AVPacket* packet,
    core::CancellationToken cancellation)
{
    return impl_->sendPacket(packet, cancellation);
}

FrameReceiveResult VideoDecoder::receiveFrame(
    const std::uint64_t sessionSerial,
    const std::int64_t presentationIndex,
    core::CancellationToken cancellation)
{
    return impl_->receive(sessionSerial, presentationIndex, cancellation);
}

void VideoDecoder::flush() noexcept
{
    impl_->flush();
}

bool VideoDecoder::usesHardwareAcceleration() const noexcept
{
    return impl_->usesHardwareAcceleration();
}

std::string VideoDecoder::hardwareDeviceName() const
{
    return impl_->hardwareDeviceName();
}

AVCodecContext* VideoDecoder::nativeHandle() noexcept
{
    return impl_->nativeHandle();
}

} // namespace vidscope::media
