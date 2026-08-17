#include "media/MediaTypes.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/mathematics.h>
}

namespace vidscope::media {
namespace {

constexpr AVRational kNanosecondTimeBase{1, 1'000'000'000};

bool isValidTimeBase(const AVRational timeBase) noexcept
{
    return timeBase.num > 0 && timeBase.den > 0;
}

std::int64_t saturatingSubtract(const std::int64_t lhs, const std::int64_t rhs) noexcept
{
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();

    if (rhs > 0 && lhs < minimum + rhs) {
        return minimum + 1; // AV_NOPTS_VALUE is reserved for "not available".
    }
    if (rhs < 0 && lhs > maximum + rhs) {
        return maximum - 1;
    }
    return lhs - rhs;
}

std::int64_t saturatingAdd(const std::int64_t lhs, const std::int64_t rhs) noexcept
{
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();

    if (rhs > 0 && lhs > maximum - rhs) {
        return maximum;
    }
    if (rhs < 0 && lhs < minimum - rhs) {
        return minimum + 1;
    }
    const auto result = lhs + rhs;
    return result == AV_NOPTS_VALUE ? minimum + 1 : result;
}

std::size_t saturatingSizeAdd(const std::size_t lhs, const std::size_t rhs) noexcept
{
    if (rhs > std::numeric_limits<std::size_t>::max() - lhs) {
        return std::numeric_limits<std::size_t>::max();
    }
    return lhs + rhs;
}

std::size_t referencedBufferBytes(const AVFrame* frame) noexcept
{
    std::size_t total = 0;

    for (int index = 0; index < AV_NUM_DATA_POINTERS; ++index) {
        const AVBufferRef* reference = frame->buf[index];
        if (reference == nullptr || reference->buffer == nullptr) {
            continue;
        }

        bool alreadyCounted = false;
        for (int previous = 0; previous < index; ++previous) {
            alreadyCounted = frame->buf[previous] != nullptr
                && frame->buf[previous]->buffer == reference->buffer;
            if (alreadyCounted) {
                break;
            }
        }
        if (!alreadyCounted) {
            total = saturatingSizeAdd(total, reference->size);
        }
    }

    for (int index = 0; index < frame->nb_extended_buf; ++index) {
        const AVBufferRef* reference = frame->extended_buf[index];
        if (reference == nullptr || reference->buffer == nullptr) {
            continue;
        }

        bool alreadyCounted = false;
        for (const AVBufferRef* previous : frame->buf) {
            alreadyCounted = previous != nullptr
                && previous->buffer == reference->buffer;
            if (alreadyCounted) {
                break;
            }
        }
        for (int previous = 0; !alreadyCounted && previous < index; ++previous) {
            alreadyCounted = frame->extended_buf[previous] != nullptr
                && frame->extended_buf[previous]->buffer == reference->buffer;
        }
        if (!alreadyCounted) {
            total = saturatingSizeAdd(total, reference->size);
        }
    }
    return total;
}

std::size_t nominalImageBytes(const AVFrame* frame) noexcept
{
    if (frame->width <= 0 || frame->height <= 0) {
        return 0;
    }

    auto format = static_cast<AVPixelFormat>(frame->format);
    if (frame->hw_frames_ctx != nullptr && frame->hw_frames_ctx->data != nullptr) {
        const auto* framesContext =
            reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
        if (framesContext->sw_format != AV_PIX_FMT_NONE) {
            format = framesContext->sw_format;
        }
    }

    const int bytes = av_image_get_buffer_size(format, frame->width, frame->height, 1);
    return bytes > 0 ? static_cast<std::size_t>(bytes) : 0;
}

[[nodiscard]] bool usableRowLayout(
    const int lineSize,
    const std::size_t rowBytes,
    const std::size_t rowCount) noexcept
{
    const auto signedLineSize = static_cast<std::int64_t>(lineSize);
    const auto magnitude = static_cast<std::uint64_t>(
        signedLineSize < 0 ? -signedLineSize : signedLineSize);
    if (magnitude < rowBytes) {
        return false;
    }
    if (rowCount <= 1 || magnitude == 0) {
        return true;
    }
    const auto maximumOffset = static_cast<std::uint64_t>(
        std::numeric_limits<std::ptrdiff_t>::max());
    return rowCount - 1 <= maximumOffset / magnitude;
}

[[nodiscard]] std::optional<double> rationalValue(const AVRational value) noexcept
{
    if (value.den == 0) {
        return std::nullopt;
    }
    return static_cast<double>(value.num) / static_cast<double>(value.den);
}

[[nodiscard]] std::optional<ChromaticityPoint> chromaticityPoint(
    const AVRational x,
    const AVRational y) noexcept
{
    const auto xValue = rationalValue(x);
    const auto yValue = rationalValue(y);
    if (!xValue || !yValue) {
        return std::nullopt;
    }
    return ChromaticityPoint{*xValue, *yValue};
}

} // namespace

FrameStorage::FrameStorage(const AVFrame* source)
{
    if (source == nullptr) {
        throw std::invalid_argument("Cannot retain a null AVFrame");
    }

    frame_ = av_frame_clone(source);
    if (frame_ == nullptr) {
        throw std::bad_alloc();
    }

    estimatedBytes_ = std::max(referencedBufferBytes(frame_), nominalImageBytes(frame_));
}

FrameStorage::~FrameStorage()
{
    av_frame_free(&frame_);
}

bool visibleImagesEqual(
    const DecodedFrame& left,
    const DecodedFrame& right) noexcept
{
    if (left.width <= 0 || left.height <= 0 || left.pixelFormat == AV_PIX_FMT_NONE
        || left.width != right.width || left.height != right.height
        || left.pixelFormat != right.pixelFormat || !left.storage || !right.storage) {
        return false;
    }

    const AVFrame* leftSurface = left.storage->get();
    const AVFrame* rightSurface = right.storage->get();
    if (leftSurface == nullptr || rightSurface == nullptr
        || leftSurface->width != left.width || rightSurface->width != right.width
        || leftSurface->height != left.height || rightSurface->height != right.height
        || leftSurface->format != left.pixelFormat || rightSurface->format != right.pixelFormat) {
        return false;
    }

    int visibleLineSizes[4]{};
    if (av_image_fill_linesizes(visibleLineSizes, left.pixelFormat, left.width) < 0) {
        return false;
    }
    const std::ptrdiff_t packedLineSizes[4] = {
        visibleLineSizes[0],
        visibleLineSizes[1],
        visibleLineSizes[2],
        visibleLineSizes[3],
    };
    std::size_t planeSizes[4]{};
    if (av_image_fill_plane_sizes(
            planeSizes, left.pixelFormat, left.height, packedLineSizes)
        < 0) {
        return false;
    }

    for (int plane = 0; plane < 4; ++plane) {
        const auto rowBytes = static_cast<std::size_t>(visibleLineSizes[plane]);
        if (rowBytes == 0) {
            if (planeSizes[plane] == 0) {
                continue;
            }
            if (leftSurface->data[plane] == nullptr || rightSurface->data[plane] == nullptr
                || std::memcmp(
                       leftSurface->data[plane],
                       rightSurface->data[plane],
                       planeSizes[plane])
                    != 0) {
                return false;
            }
            continue;
        }
        if (planeSizes[plane] == 0 || planeSizes[plane] % rowBytes != 0) {
            return false;
        }

        const auto rowCount = planeSizes[plane] / rowBytes;
        if (leftSurface->data[plane] == nullptr || rightSurface->data[plane] == nullptr
            || !usableRowLayout(leftSurface->linesize[plane], rowBytes, rowCount)
            || !usableRowLayout(rightSurface->linesize[plane], rowBytes, rowCount)) {
            return false;
        }

        for (std::size_t rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
            const auto row = static_cast<std::ptrdiff_t>(rowIndex);
            const auto* leftRow = leftSurface->data[plane]
                + row * static_cast<std::ptrdiff_t>(leftSurface->linesize[plane]);
            const auto* rightRow = rightSurface->data[plane]
                + row * static_cast<std::ptrdiff_t>(rightSurface->linesize[plane]);
            if (std::memcmp(leftRow, rightRow, rowBytes) != 0) {
                return false;
            }
        }
    }
    return true;
}

std::optional<MasteringDisplayMetadata> extractMasteringDisplayMetadata(
    const AVFrame& frame) noexcept
{
    const AVFrameSideData* sideData =
        av_frame_get_side_data(&frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (sideData == nullptr || sideData->data == nullptr
        || sideData->size < sizeof(AVMasteringDisplayMetadata)) {
        return std::nullopt;
    }

    const auto* source =
        reinterpret_cast<const AVMasteringDisplayMetadata*>(sideData->data);
    MasteringDisplayMetadata result;
    if (source->has_primaries != 0) {
        const auto red = chromaticityPoint(
            source->display_primaries[0][0], source->display_primaries[0][1]);
        const auto green = chromaticityPoint(
            source->display_primaries[1][0], source->display_primaries[1][1]);
        const auto blue = chromaticityPoint(
            source->display_primaries[2][0], source->display_primaries[2][1]);
        const auto whitePoint = chromaticityPoint(
            source->white_point[0], source->white_point[1]);
        if (red && green && blue && whitePoint) {
            result.primaries = MasteringDisplayPrimaries{
                *red,
                *green,
                *blue,
                *whitePoint,
            };
        }
    }
    if (source->has_luminance != 0) {
        const auto minimum = rationalValue(source->min_luminance);
        const auto maximum = rationalValue(source->max_luminance);
        if (minimum && maximum) {
            result.luminance = MasteringDisplayLuminance{*minimum, *maximum};
        }
    }

    if (!result.primaries && !result.luminance) {
        return std::nullopt;
    }
    return result;
}

std::optional<ContentLightMetadata> extractContentLightMetadata(
    const AVFrame& frame) noexcept
{
    const AVFrameSideData* sideData =
        av_frame_get_side_data(&frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (sideData == nullptr || sideData->data == nullptr
        || sideData->size < sizeof(AVContentLightMetadata)) {
        return std::nullopt;
    }

    const auto* source = reinterpret_cast<const AVContentLightMetadata*>(sideData->data);
    return ContentLightMetadata{
        static_cast<std::uint32_t>(source->MaxCLL),
        static_cast<std::uint32_t>(source->MaxFALL),
    };
}

MediaTime timestampToMediaTime(
    const std::int64_t timestamp,
    const std::int64_t originTimestamp,
    const AVRational timeBase) noexcept
{
    if (timestamp == AV_NOPTS_VALUE || originTimestamp == AV_NOPTS_VALUE
        || !isValidTimeBase(timeBase)) {
        return kNoMediaTime;
    }

    const std::int64_t relativeTimestamp = saturatingSubtract(timestamp, originTimestamp);
    std::int64_t nanoseconds = av_rescale_q_rnd(
        relativeTimestamp,
        timeBase,
        kNanosecondTimeBase,
        static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    if (nanoseconds == AV_NOPTS_VALUE) {
        nanoseconds = std::numeric_limits<std::int64_t>::min() + 1;
    }
    return MediaTime{nanoseconds};
}

std::int64_t mediaTimeToTimestamp(
    const MediaTime time,
    const std::int64_t originTimestamp,
    const AVRational timeBase) noexcept
{
    if (time == kNoMediaTime || originTimestamp == AV_NOPTS_VALUE
        || !isValidTimeBase(timeBase)) {
        return AV_NOPTS_VALUE;
    }

    std::int64_t relativeTimestamp = av_rescale_q_rnd(
        time.count(),
        kNanosecondTimeBase,
        timeBase,
        static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    if (relativeTimestamp == AV_NOPTS_VALUE) {
        relativeTimestamp = std::numeric_limits<std::int64_t>::min() + 1;
    }
    return saturatingAdd(originTimestamp, relativeTimestamp);
}

MediaTime nominalFrameDuration(const AVRational frameRate) noexcept
{
    if (frameRate.num <= 0 || frameRate.den <= 0) {
        return MediaTime::zero();
    }

    const std::int64_t nanoseconds = av_rescale_q_rnd(
        1,
        av_inv_q(frameRate),
        kNanosecondTimeBase,
        AV_ROUND_NEAR_INF);
    return nanoseconds > 0 ? MediaTime{nanoseconds} : MediaTime::zero();
}

const char* pictureTypeName(const AVPictureType type) noexcept
{
    switch (type) {
    case AV_PICTURE_TYPE_NONE:
        return "None";
    case AV_PICTURE_TYPE_I:
        return "I";
    case AV_PICTURE_TYPE_P:
        return "P";
    case AV_PICTURE_TYPE_B:
        return "B";
    case AV_PICTURE_TYPE_S:
        return "S";
    case AV_PICTURE_TYPE_SI:
        return "SI";
    case AV_PICTURE_TYPE_SP:
        return "SP";
    case AV_PICTURE_TYPE_BI:
        return "BI";
    default:
        return "Unknown";
    }
}

} // namespace vidscope::media
