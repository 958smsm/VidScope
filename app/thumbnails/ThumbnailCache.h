#pragma once

#include "media/MediaTypes.h"
#include "thumbnails/ThumbnailTypes.h"

#include <QtCore/QString>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace vidscope::thumbnails {

struct ThumbnailCacheKey final {
    QString mediaIdentity;
    qint64 timestampNanoseconds = 0;
    QSize targetSize;

    friend bool operator==(const ThumbnailCacheKey&, const ThumbnailCacheKey&) = default;
};

struct ThumbnailCacheConfig final {
    std::size_t memoryBudgetBytes = 96ULL * 1024ULL * 1024ULL;
    std::size_t diskBudgetBytes = 1024ULL * 1024ULL * 1024ULL;
    QString diskDirectory;
};

struct ThumbnailCacheLookup final {
    ThumbnailFrame frame;
    ThumbnailCacheSource source = ThumbnailCacheSource::None;
};

struct ThumbnailCacheStats final {
    std::size_t memoryEntries = 0;
    std::size_t memoryBytes = 0;
    std::uint64_t memoryHits = 0;
    std::uint64_t diskHits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;
    std::uint64_t diskWrites = 0;
};

class ThumbnailCache final {
public:
    explicit ThumbnailCache(ThumbnailCacheConfig config = {});
    ~ThumbnailCache();
    ThumbnailCache(const ThumbnailCache&) = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    [[nodiscard]] static QString mediaIdentity(const media::MediaInfo& info);

    [[nodiscard]] std::optional<ThumbnailFrame> lookupMemory(const ThumbnailCacheKey& key);
    [[nodiscard]] std::optional<ThumbnailFrame> lookup(const ThumbnailCacheKey& key);
    [[nodiscard]] std::optional<ThumbnailCacheLookup> lookupWithSource(
        const ThumbnailCacheKey& key);
    void insertMemory(const ThumbnailCacheKey& key, const ThumbnailFrame& frame);
    void insertDisk(const ThumbnailCacheKey& key, const ThumbnailFrame& frame);
    void insert(const ThumbnailCacheKey& key, const ThumbnailFrame& frame);

    void clearMemory();
    void clearMedia(const QString& mediaIdentity);
    void pruneDisk();

    [[nodiscard]] ThumbnailCacheStats stats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vidscope::thumbnails
