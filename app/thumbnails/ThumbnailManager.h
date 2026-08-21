#pragma once

#include "media/MediaTypes.h"
#include "media/VideoDecoder.h"
#include "thumbnails/ThumbnailCache.h"
#include "thumbnails/ThumbnailTypes.h"

#include <QtCore/QObject>
#include <QtCore/QSize>
#include <QtCore/QString>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace vidscope::thumbnails {

struct ThumbnailManagerConfig final {
    std::size_t workerCount = 2;
    std::size_t maximumPendingRequests = 64;
    std::size_t workerFrameCacheBytes = 48ULL * 1024ULL * 1024ULL;
    std::size_t workerQueueBytes = 16ULL * 1024ULL * 1024ULL;
    std::size_t workerQueueFrames = 4;
    QSize maximumThumbnailSize{640, 360};
    media::HardwareAcceleration hardwareAcceleration = media::HardwareAcceleration::Disabled;
    ThumbnailCacheConfig cache;
};

class ThumbnailManager final : public QObject {
    Q_OBJECT

public:
    explicit ThumbnailManager(ThumbnailManagerConfig config = {}, QObject* parent = nullptr);
    ~ThumbnailManager() override;
    ThumbnailManager(const ThumbnailManager&) = delete;
    ThumbnailManager& operator=(const ThumbnailManager&) = delete;

    void setMedia(media::MediaInfoPtr info);
    void clearMedia();

    [[nodiscard]] ThumbnailGeneration requestPreview(
        qint64 timestampNanoseconds,
        QSize targetSize = QSize(320, 180),
        ThumbnailPriority priority = ThumbnailPriority::HoverPreview,
        qint64 presentationIndexHint = -1);
    void cancelHoverPreview() noexcept;
    void cancelAll() noexcept;

    [[nodiscard]] ThumbnailCacheStats cacheStats() const;

signals:
    void previewReady(const vidscope::thumbnails::ThumbnailResult& result);
    void previewFailed(vidscope::thumbnails::ThumbnailGeneration generation, const QString& detail);
    void cacheStatsChanged(
        quint64 memoryHits,
        quint64 diskHits,
        quint64 misses,
        qsizetype memoryEntries);

private:
    void deliverResult(ThumbnailResult result, std::uint64_t mediaEpoch);
    void deliverFailure(
        ThumbnailGeneration generation,
        ThumbnailPriority priority,
        std::uint64_t mediaEpoch,
        QString detail);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vidscope::thumbnails
