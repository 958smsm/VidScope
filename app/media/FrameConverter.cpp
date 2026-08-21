#include "media/FrameConverter.h"

#include "core/Logging.h"
#include "media/FfmpegRaii.h"

#include <QtCore/QtGlobal>

#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace vidscope::media {
namespace {

int swscaleColorSpace(const AVColorSpace colorSpace) noexcept
{
    switch (colorSpace) {
    case AVCOL_SPC_BT709:
        return SWS_CS_ITU709;
    case AVCOL_SPC_FCC:
        return SWS_CS_FCC;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        return SWS_CS_ITU601;
    case AVCOL_SPC_SMPTE240M:
        return SWS_CS_SMPTE240M;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return SWS_CS_BT2020;
    default:
        return SWS_CS_DEFAULT;
    }
}

bool isHardwarePixelFormat(const AVPixelFormat format) noexcept
{
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(format);
    return descriptor != nullptr && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0;
}

#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
constexpr AVPixelFormat kOutputPixelFormat = AV_PIX_FMT_BGRA;
constexpr QImage::Format kOutputImageFormat = QImage::Format_ARGB32;
#else
// RGBA8888 is byte ordered, unlike ARGB32's native-endian word layout.
constexpr AVPixelFormat kOutputPixelFormat = AV_PIX_FMT_RGBA;
constexpr QImage::Format kOutputImageFormat = QImage::Format_RGBA8888;
#endif

} // namespace

class FrameConverter::Impl final {
public:
    void reset() noexcept
    {
        context.reset();
    }

    SwsContextPtr context;
};

FrameConverter::FrameConverter()
    : impl_(std::make_unique<Impl>())
{
}

FrameConverter::~FrameConverter() = default;

QImage FrameConverter::toBgraImage(
    const DecodedFrame& frame,
    core::CancellationToken cancellation)
{
    return toBgraImage(frame, QSize(frame.width, frame.height), std::move(cancellation));
}

QImage FrameConverter::toBgraImage(
    const DecodedFrame& frame,
    QSize outputSize,
    core::CancellationToken cancellation)
{
    if (cancellation.isCancellationRequested()) {
        return {};
    }
    if (!frame.storage || frame.storage->get() == nullptr) {
        throw std::invalid_argument("The decoded frame has no retained image storage");
    }

    const AVFrame* source = frame.storage->get();
    if (source->width <= 0 || source->height <= 0 || source->data[0] == nullptr
        || source->format < 0) {
        throw std::invalid_argument("The decoded frame has invalid image geometry or data");
    }
    if (!outputSize.isValid() || outputSize.width() <= 0 || outputSize.height() <= 0) {
        throw std::invalid_argument("The requested converted image size is invalid");
    }
    const auto sourceFormat = static_cast<AVPixelFormat>(source->format);
    if (isHardwarePixelFormat(sourceFormat) || source->hw_frames_ctx != nullptr) {
        throw std::invalid_argument(
            "A hardware frame must be transferred to system memory before conversion");
    }

    SwsContext* previousContext = impl_->context.release();
    SwsContext* context = sws_getCachedContext(
        previousContext,
        source->width,
        source->height,
        sourceFormat,
        outputSize.width(),
        outputSize.height(),
        kOutputPixelFormat,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr);
    impl_->context.reset(context);
    if (context == nullptr) {
        throw std::runtime_error("Could not initialize FFmpeg color conversion");
    }

    const int colorSpace = swscaleColorSpace(frame.colorSpace);
    const int* coefficients = sws_getCoefficients(colorSpace);
    if (coefficients != nullptr) {
        const int sourceIsFullRange = frame.colorRange == AVCOL_RANGE_JPEG ? 1 : 0;
        const int colorResult = sws_setColorspaceDetails(
            context,
            coefficients,
            sourceIsFullRange,
            coefficients,
            1,
            0,
            1 << 16,
            1 << 16);
        if (colorResult < 0) {
            qCDebug(logRender) << "FFmpeg ignored explicit frame colorspace details";
        }
    }

    QImage image(outputSize, kOutputImageFormat);
    if (image.isNull()) {
        throw std::runtime_error("Could not allocate the converted video image");
    }
    if (cancellation.isCancellationRequested()) {
        return {};
    }

    std::array<std::uint8_t*, 4> outputData{image.bits(), nullptr, nullptr, nullptr};
    if (image.bytesPerLine() > std::numeric_limits<int>::max()) {
        throw std::runtime_error("The converted video image stride is too large");
    }
    std::array<int, 4> outputLinesize{
        static_cast<int>(image.bytesPerLine()), 0, 0, 0};
    const int convertedRows = sws_scale(
        context,
        source->data,
        source->linesize,
        0,
        source->height,
        outputData.data(),
        outputLinesize.data());
    if (convertedRows != outputSize.height()) {
        throw std::runtime_error("FFmpeg did not convert the complete video frame");
    }
    if (cancellation.isCancellationRequested()) {
        return {};
    }
    return image;
}

void FrameConverter::reset() noexcept
{
    impl_->reset();
}

} // namespace vidscope::media
