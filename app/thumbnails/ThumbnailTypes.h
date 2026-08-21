#pragma once

#include "media/MediaTypes.h"

#include <QtCore/QMetaType>
#include <QtCore/QSize>
#include <QtCore/QtTypes>
#include <QtGui/QImage>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace vidscope::thumbnails {

using ThumbnailGeneration = quint64;

enum class ThumbnailPriority : std::uint8_t {
    HoverPreview = 0,
    UserRequested = 1,
    VisibleThumbnail = 2,
    NearPlayhead = 3,
    BackgroundPrecache = 4,
};

enum class ThumbnailCacheSource : std::uint8_t {
    None,
    Memory,
    Disk,
    Decoded,
};

struct ThumbnailRequest final {
    ThumbnailGeneration generation = 0;
    media::MediaTime timestamp{};
    QSize targetSize{320, 180};
    ThumbnailPriority priority = ThumbnailPriority::HoverPreview;
    std::int64_t presentationIndexHint = -1;
};

struct ThumbnailFrame final {
    QImage image;
    media::MediaTime presentationTime{};
    media::MediaTime duration{};
    std::int64_t presentationIndex = -1;
    std::int64_t pts = AV_NOPTS_VALUE;
    std::int64_t dts = AV_NOPTS_VALUE;
    bool keyFrame = false;
    AVPictureType pictureType = AV_PICTURE_TYPE_NONE;
    std::optional<float> motionScore;
    std::optional<float> similarityScore;

    [[nodiscard]] std::size_t estimatedBytes() const noexcept
    {
        const qsizetype byteCount = image.sizeInBytes();
        return byteCount > 0 ? static_cast<std::size_t>(byteCount) : 0U;
    }
};

struct ThumbnailResult final {
    ThumbnailRequest request;
    ThumbnailFrame frame;
    ThumbnailCacheSource cacheSource = ThumbnailCacheSource::None;
    qint64 latencyMicroseconds = 0;
};

} // namespace vidscope::thumbnails

Q_DECLARE_METATYPE(vidscope::thumbnails::ThumbnailCacheSource)
Q_DECLARE_METATYPE(vidscope::thumbnails::ThumbnailRequest)
Q_DECLARE_METATYPE(vidscope::thumbnails::ThumbnailResult)
