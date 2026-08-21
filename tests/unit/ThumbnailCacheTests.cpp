#include "TestHarness.h"

#include "thumbnails/ThumbnailCache.h"

#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QtGlobal>
#include <QtGui/QImage>

#include <chrono>
#include <filesystem>

using namespace std::chrono_literals;

namespace {

using vidscope::thumbnails::ThumbnailCache;
using vidscope::thumbnails::ThumbnailCacheConfig;
using vidscope::thumbnails::ThumbnailCacheKey;
using vidscope::thumbnails::ThumbnailCacheSource;
using vidscope::thumbnails::ThumbnailFrame;

[[nodiscard]] ThumbnailCacheKey cacheKey(
    qint64 timestampNanoseconds,
    QSize size = QSize(8, 8))
{
    return {QStringLiteral("0123456789abcdef"), timestampNanoseconds, size};
}

[[nodiscard]] ThumbnailFrame frameWithValue(int value, QSize size = QSize(8, 8))
{
    ThumbnailFrame frame;
    frame.image = QImage(size, QImage::Format_ARGB32);
    frame.image.fill(qRgba(value, value + 1, value + 2, 255));
    frame.presentationTime = vidscope::media::MediaTime(value * 1'000'000LL);
    frame.duration = 40ms;
    frame.presentationIndex = value;
    frame.pts = value * 3;
    frame.dts = value * 2;
    frame.keyFrame = value % 2 == 0;
    frame.pictureType = frame.keyFrame ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_P;
    return frame;
}

[[nodiscard]] std::filesystem::path pathFromQString(const QString& path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}

} // namespace

VIDSCOPE_TEST(ThumbnailCache_enforces_memory_budget_with_lru_eviction)
{
    const ThumbnailFrame sample = frameWithValue(10);
    const std::size_t bytes = sample.estimatedBytes();
    VIDSCOPE_REQUIRE(bytes > 0);

    ThumbnailCacheConfig config;
    config.memoryBudgetBytes = bytes * 2;
    config.diskBudgetBytes = 0;
    ThumbnailCache cache(config);

    const auto firstKey = cacheKey(1);
    const auto secondKey = cacheKey(2);
    const auto thirdKey = cacheKey(3);
    cache.insert(firstKey, sample);
    cache.insert(secondKey, frameWithValue(20));
    VIDSCOPE_REQUIRE(cache.lookupMemory(firstKey).has_value()); // First becomes MRU.
    cache.insert(thirdKey, frameWithValue(30));

    VIDSCOPE_REQUIRE(cache.lookupMemory(firstKey).has_value());
    VIDSCOPE_REQUIRE(!cache.lookupMemory(secondKey).has_value());
    VIDSCOPE_REQUIRE(cache.lookupMemory(thirdKey).has_value());

    const auto stats = cache.stats();
    VIDSCOPE_REQUIRE(stats.memoryEntries == 2);
    VIDSCOPE_REQUIRE(stats.memoryBytes <= config.memoryBudgetBytes);
    VIDSCOPE_REQUIRE(stats.evictions == 1);
}

VIDSCOPE_TEST(ThumbnailCache_round_trips_image_and_exact_metadata_through_disk)
{
    QTemporaryDir directory;
    VIDSCOPE_REQUIRE(directory.isValid());

    ThumbnailCacheConfig config;
    config.memoryBudgetBytes = 0;
    config.diskBudgetBytes = 8U * 1024U * 1024U;
    config.diskDirectory = directory.path();

    const auto key = cacheKey(1'234'567'890LL, QSize(16, 9));
    ThumbnailFrame original = frameWithValue(42, QSize(16, 9));
    original.presentationTime = 1'250ms;
    original.duration = 83ms;
    original.presentationIndex = 15;
    original.pts = 18'000;
    original.dts = 15'000;
    original.keyFrame = false;
    original.pictureType = AV_PICTURE_TYPE_B;
    original.motionScore = 0.375F;
    original.similarityScore = 0.875F;

    {
        ThumbnailCache writer(config);
        writer.insert(key, original);
        VIDSCOPE_REQUIRE(writer.stats().diskWrites == 1);
    }

    ThumbnailCache reader(config);
    const auto restored = reader.lookupWithSource(key);
    VIDSCOPE_REQUIRE(restored.has_value());
    VIDSCOPE_REQUIRE(restored->source == ThumbnailCacheSource::Disk);
    VIDSCOPE_REQUIRE(restored->frame.image == original.image);
    VIDSCOPE_REQUIRE(restored->frame.presentationTime == original.presentationTime);
    VIDSCOPE_REQUIRE(restored->frame.duration == original.duration);
    VIDSCOPE_REQUIRE(restored->frame.presentationIndex == original.presentationIndex);
    VIDSCOPE_REQUIRE(restored->frame.pts == original.pts);
    VIDSCOPE_REQUIRE(restored->frame.dts == original.dts);
    VIDSCOPE_REQUIRE(restored->frame.keyFrame == original.keyFrame);
    VIDSCOPE_REQUIRE(restored->frame.pictureType == original.pictureType);
    VIDSCOPE_REQUIRE(restored->frame.motionScore == original.motionScore);
    VIDSCOPE_REQUIRE(restored->frame.similarityScore == original.similarityScore);
    VIDSCOPE_REQUIRE(reader.stats().diskHits == 1);
}

VIDSCOPE_TEST(ThumbnailCache_memory_and_disk_insert_paths_remain_independent)
{
    QTemporaryDir directory;
    VIDSCOPE_REQUIRE(directory.isValid());

    ThumbnailCacheConfig config;
    config.memoryBudgetBytes = 2U * 1024U * 1024U;
    config.diskBudgetBytes = 8U * 1024U * 1024U;
    config.diskDirectory = directory.path();

    const auto key = cacheKey(2'000'000'000LL, QSize(24, 14));
    const auto original = frameWithValue(51, QSize(24, 14));

    ThumbnailCache writer(config);
    writer.insertMemory(key, original);
    VIDSCOPE_REQUIRE(writer.lookupMemory(key).has_value());
    VIDSCOPE_REQUIRE(writer.stats().diskWrites == 0);

    ThumbnailCache beforeDiskWrite(config);
    VIDSCOPE_REQUIRE(!beforeDiskWrite.lookupWithSource(key).has_value());

    writer.insertDisk(key, original);
    VIDSCOPE_REQUIRE(writer.stats().diskWrites == 1);

    ThumbnailCache afterDiskWrite(config);
    const auto restored = afterDiskWrite.lookupWithSource(key);
    VIDSCOPE_REQUIRE(restored.has_value());
    VIDSCOPE_REQUIRE(restored->source == ThumbnailCacheSource::Disk);
    VIDSCOPE_REQUIRE(restored->frame.image == original.image);
}

VIDSCOPE_TEST(ThumbnailCache_media_identity_changes_when_the_source_file_changes)
{
    QTemporaryDir directory;
    VIDSCOPE_REQUIRE(directory.isValid());

    const QString filePath = directory.filePath(QStringLiteral("identity.bin"));
    QFile file(filePath);
    VIDSCOPE_REQUIRE(file.open(QIODevice::WriteOnly));
    VIDSCOPE_REQUIRE(file.write("first", 5) == 5);
    file.close();

    vidscope::media::MediaInfo info;
    info.path = pathFromQString(filePath);
    info.videoStreamIndex = 0;
    const QString firstIdentity = ThumbnailCache::mediaIdentity(info);
    VIDSCOPE_REQUIRE(!firstIdentity.isEmpty());

    VIDSCOPE_REQUIRE(file.open(QIODevice::Append));
    VIDSCOPE_REQUIRE(file.write("-changed", 8) == 8);
    file.close();

    const QString changedIdentity = ThumbnailCache::mediaIdentity(info);
    VIDSCOPE_REQUIRE(!changedIdentity.isEmpty());
    VIDSCOPE_REQUIRE(firstIdentity != changedIdentity);

    info.videoStreamIndex = 1;
    VIDSCOPE_REQUIRE(ThumbnailCache::mediaIdentity(info) != changedIdentity);
}
