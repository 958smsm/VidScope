#include "thumbnails/ThumbnailCache.h"

#include "core/Logging.h"

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDataStream>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHashFunctions>
#include <QtCore/QSaveFile>
#include <QtCore/QStringList>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <iterator>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vidscope::thumbnails {
namespace {

constexpr quint32 kDiskMagic = 0x56535448U; // "VSTH"
constexpr quint16 kDiskSchemaVersion = 1U;
constexpr int kMaximumCachedImageDimension = 4'096;
constexpr qint64 kMaximumDiskEntryBytes = 32LL * 1024LL * 1024LL;
constexpr std::uint64_t kPruneIntervalWrites = 16U;

[[nodiscard]] QString pathToQString(const std::filesystem::path& path)
{
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

[[nodiscard]] QByteArray stableHash(QByteArray input)
{
    return QCryptographicHash::hash(std::move(input), QCryptographicHash::Sha256).toHex();
}

void appendIdentityPart(QByteArray& destination, const QByteArray& part)
{
    destination.append(QByteArray::number(static_cast<qlonglong>(part.size())));
    destination.append(':');
    destination.append(part);
    destination.append('|');
}

[[nodiscard]] std::size_t safeFileSize(qint64 size) noexcept
{
    if (size <= 0) {
        return 0U;
    }
    const auto unsignedSize = static_cast<quint64>(size);
    const auto maximum = static_cast<quint64>(std::numeric_limits<std::size_t>::max());
    return static_cast<std::size_t>(std::min(unsignedSize, maximum));
}

} // namespace

class ThumbnailCache::Impl final {
public:
    explicit Impl(ThumbnailCacheConfig config)
        : config_(std::move(config))
    {
        if (!config_.diskDirectory.isEmpty() && config_.diskBudgetBytes > 0U) {
            (void)QDir().mkpath(config_.diskDirectory);
            pruneDiskLocked();
        }
    }

    [[nodiscard]] std::optional<ThumbnailFrame> lookupMemory(const ThumbnailCacheKey& key)
    {
        std::lock_guard lock(memoryMutex_);
        return lookupMemoryLocked(key);
    }

    [[nodiscard]] std::optional<ThumbnailFrame> lookup(const ThumbnailCacheKey& key)
    {
        if (auto found = lookupWithSource(key)) {
            return std::move(found->frame);
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<ThumbnailCacheLookup> lookupWithSource(
        const ThumbnailCacheKey& key)
    {
        if (auto memory = lookupMemory(key)) {
            return ThumbnailCacheLookup{std::move(*memory), ThumbnailCacheSource::Memory};
        }

        std::optional<ThumbnailFrame> disk;
        {
            std::lock_guard lock(diskMutex_);
            disk = lookupDiskLocked(key);
        }
        if (disk) {
            diskHits_.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard lock(memoryMutex_);
                insertMemoryLocked(key, *disk);
            }
            return ThumbnailCacheLookup{std::move(*disk), ThumbnailCacheSource::Disk};
        }

        misses_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    void insertMemory(const ThumbnailCacheKey& key, const ThumbnailFrame& frame)
    {
        if (frame.image.isNull() || key.mediaIdentity.isEmpty() || !key.targetSize.isValid()) {
            return;
        }

        std::lock_guard lock(memoryMutex_);
        insertMemoryLocked(key, frame);
    }

    void insertDisk(const ThumbnailCacheKey& key, const ThumbnailFrame& frame)
    {
        if (frame.image.isNull() || key.mediaIdentity.isEmpty() || !key.targetSize.isValid()) {
            return;
        }

        std::lock_guard lock(diskMutex_);
        writeDiskLocked(key, frame);
    }

    void insert(const ThumbnailCacheKey& key, const ThumbnailFrame& frame)
    {
        insertMemory(key, frame);
        insertDisk(key, frame);
    }

    void clearMemory()
    {
        std::lock_guard lock(memoryMutex_);
        entries_.clear();
        index_.clear();
        memoryBytes_ = 0;
    }

    void clearMedia(const QString& mediaIdentity)
    {
        if (mediaIdentity.isEmpty()) {
            return;
        }

        {
            std::lock_guard lock(memoryMutex_);
            for (auto entry = entries_.begin(); entry != entries_.end();) {
                if (entry->key.mediaIdentity != mediaIdentity) {
                    ++entry;
                    continue;
                }
                memoryBytes_ -= std::min(memoryBytes_, entry->bytes);
                index_.erase(entry->key);
                entry = entries_.erase(entry);
            }
        }

        if (!config_.diskDirectory.isEmpty()) {
            std::lock_guard lock(diskMutex_);
            QDir mediaDirectory(QDir(config_.diskDirectory).filePath(mediaIdentity));
            if (mediaDirectory.exists() && !mediaDirectory.removeRecursively()) {
                qCWarning(logCache) << "Could not remove thumbnail cache directory"
                                    << mediaDirectory.absolutePath();
            }
        }
    }

    void pruneDisk()
    {
        std::lock_guard lock(diskMutex_);
        pruneDiskLocked();
    }

    [[nodiscard]] ThumbnailCacheStats stats() const
    {
        ThumbnailCacheStats snapshot;
        {
            std::lock_guard lock(memoryMutex_);
            snapshot.memoryEntries = entries_.size();
            snapshot.memoryBytes = memoryBytes_;
        }
        snapshot.memoryHits = memoryHits_.load(std::memory_order_relaxed);
        snapshot.diskHits = diskHits_.load(std::memory_order_relaxed);
        snapshot.misses = misses_.load(std::memory_order_relaxed);
        snapshot.evictions = evictions_.load(std::memory_order_relaxed);
        snapshot.diskWrites = diskWrites_.load(std::memory_order_relaxed);
        return snapshot;
    }

private:
    struct KeyHash final {
        [[nodiscard]] std::size_t operator()(const ThumbnailCacheKey& key) const noexcept
        {
            std::size_t seed = qHash(key.mediaIdentity);
            seed ^= qHash(key.timestampNanoseconds, seed + std::size_t{0x9e3779b9U});
            seed ^= qHash(key.targetSize.width(), seed + std::size_t{0x9e3779b9U});
            seed ^= qHash(key.targetSize.height(), seed + std::size_t{0x9e3779b9U});
            return seed;
        }
    };

    struct MemoryEntry final {
        ThumbnailCacheKey key;
        ThumbnailFrame frame;
        std::size_t bytes = 0;
    };

    using EntryList = std::list<MemoryEntry>;
    using EntryIterator = EntryList::iterator;

    [[nodiscard]] std::optional<ThumbnailFrame> lookupMemoryLocked(const ThumbnailCacheKey& key)
    {
        const auto found = index_.find(key);
        if (found == index_.end()) {
            return std::nullopt;
        }
        entries_.splice(entries_.begin(), entries_, found->second);
        found->second = entries_.begin();
        memoryHits_.fetch_add(1, std::memory_order_relaxed);
        return entries_.front().frame;
    }

    void insertMemoryLocked(const ThumbnailCacheKey& key, const ThumbnailFrame& frame)
    {
        if (config_.memoryBudgetBytes == 0U) {
            return;
        }

        const std::size_t bytes = frame.estimatedBytes();
        if (bytes == 0U || bytes > config_.memoryBudgetBytes) {
            return;
        }

        if (const auto found = index_.find(key); found != index_.end()) {
            memoryBytes_ -= std::min(memoryBytes_, found->second->bytes);
            entries_.erase(found->second);
            index_.erase(found);
        }

        entries_.push_front(MemoryEntry{key, frame, bytes});
        index_.emplace(entries_.front().key, entries_.begin());
        memoryBytes_ += bytes;

        while (memoryBytes_ > config_.memoryBudgetBytes && !entries_.empty()) {
            auto last = std::prev(entries_.end());
            memoryBytes_ -= std::min(memoryBytes_, last->bytes);
            index_.erase(last->key);
            entries_.erase(last);
            evictions_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] QString diskFilePathLocked(const ThumbnailCacheKey& key) const
    {
        if (config_.diskDirectory.isEmpty() || key.mediaIdentity.isEmpty()) {
            return {};
        }

        QByteArray identity;
        appendIdentityPart(identity, key.mediaIdentity.toUtf8());
        appendIdentityPart(identity, QByteArray::number(key.timestampNanoseconds));
        appendIdentityPart(identity, QByteArray::number(key.targetSize.width()));
        appendIdentityPart(identity, QByteArray::number(key.targetSize.height()));
        appendIdentityPart(identity, QByteArray::number(kDiskSchemaVersion));

        const QString fileName = QString::fromLatin1(stableHash(std::move(identity)))
            + QStringLiteral(".vsthumb");
        return QDir(config_.diskDirectory)
            .filePath(key.mediaIdentity + QLatin1Char('/') + fileName);
    }

    [[nodiscard]] std::optional<ThumbnailFrame> lookupDiskLocked(const ThumbnailCacheKey& key)
    {
        if (config_.diskBudgetBytes == 0U) {
            return std::nullopt;
        }
        const QString filePath = diskFilePathLocked(key);
        if (filePath.isEmpty()) {
            return std::nullopt;
        }

        const QFileInfo cachedFile(filePath);
        if (cachedFile.size() <= 0 || cachedFile.size() > kMaximumDiskEntryBytes) {
            if (cachedFile.exists()) {
                qCWarning(logCache) << "Discarding oversized thumbnail cache entry" << filePath;
                (void)QFile::remove(filePath);
            }
            return std::nullopt;
        }

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            return std::nullopt;
        }

        QDataStream stream(&file);
        stream.setVersion(QDataStream::Qt_6_0);

        quint32 magic = 0;
        quint16 schemaVersion = 0;
        qint64 presentationTime = 0;
        qint64 duration = 0;
        qint64 presentationIndex = -1;
        qint64 pts = AV_NOPTS_VALUE;
        qint64 dts = AV_NOPTS_VALUE;
        bool keyFrame = false;
        qint32 pictureType = static_cast<qint32>(AV_PICTURE_TYPE_NONE);
        bool hasMotion = false;
        float motion = 0.0F;
        bool hasSimilarity = false;
        float similarity = 0.0F;
        QImage image;

        stream >> magic >> schemaVersion >> presentationTime >> duration >> presentationIndex
            >> pts >> dts >> keyFrame >> pictureType >> hasMotion >> motion
            >> hasSimilarity >> similarity >> image;
        if (stream.status() == QDataStream::Ok) {
            (void)file.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileModificationTime);
        }
        file.close();

        const bool valid = stream.status() == QDataStream::Ok
            && magic == kDiskMagic
            && schemaVersion == kDiskSchemaVersion
            && !image.isNull()
            && image.width() > 0
            && image.height() > 0
            && image.width() <= kMaximumCachedImageDimension
            && image.height() <= kMaximumCachedImageDimension;
        if (!valid) {
            qCWarning(logCache) << "Discarding invalid thumbnail cache entry" << filePath;
            (void)QFile::remove(filePath);
            return std::nullopt;
        }

        ThumbnailFrame frame;
        frame.image = std::move(image);
        frame.presentationTime = media::MediaTime(presentationTime);
        frame.duration = media::MediaTime(duration);
        frame.presentationIndex = presentationIndex;
        frame.pts = pts;
        frame.dts = dts;
        frame.keyFrame = keyFrame;
        frame.pictureType = static_cast<AVPictureType>(pictureType);
        if (hasMotion) {
            frame.motionScore = motion;
        }
        if (hasSimilarity) {
            frame.similarityScore = similarity;
        }
        return frame;
    }

    void writeDiskLocked(const ThumbnailCacheKey& key, const ThumbnailFrame& frame)
    {
        if (config_.diskBudgetBytes == 0U || config_.diskDirectory.isEmpty()) {
            return;
        }

        const QString filePath = diskFilePathLocked(key);
        if (filePath.isEmpty()) {
            return;
        }
        const QFileInfo fileInfo(filePath);
        if (!QDir().mkpath(fileInfo.absolutePath())) {
            qCWarning(logCache) << "Could not create thumbnail cache directory"
                                << fileInfo.absolutePath();
            return;
        }

        QSaveFile file(filePath);
        if (!file.open(QIODevice::WriteOnly)) {
            qCWarning(logCache) << "Could not open thumbnail cache file for writing" << filePath;
            return;
        }

        QDataStream stream(&file);
        stream.setVersion(QDataStream::Qt_6_0);
        stream << kDiskMagic << kDiskSchemaVersion
               << static_cast<qint64>(frame.presentationTime.count())
               << static_cast<qint64>(frame.duration.count())
               << static_cast<qint64>(frame.presentationIndex)
               << static_cast<qint64>(frame.pts)
               << static_cast<qint64>(frame.dts)
               << frame.keyFrame
               << static_cast<qint32>(frame.pictureType)
               << frame.motionScore.has_value()
               << frame.motionScore.value_or(0.0F)
               << frame.similarityScore.has_value()
               << frame.similarityScore.value_or(0.0F)
               << frame.image;

        if (stream.status() != QDataStream::Ok || !file.commit()) {
            qCWarning(logCache) << "Could not commit thumbnail cache file" << filePath;
            return;
        }

        diskWrites_.fetch_add(1, std::memory_order_relaxed);
        ++writesSincePrune_;
        if (writesSincePrune_ >= kPruneIntervalWrites) {
            pruneDiskLocked();
        }
    }

    void pruneDiskLocked()
    {
        writesSincePrune_ = 0;
        if (config_.diskDirectory.isEmpty() || config_.diskBudgetBytes == 0U) {
            return;
        }

        struct DiskEntry final {
            QString path;
            qint64 modifiedMilliseconds = 0;
            std::size_t bytes = 0;
        };

        std::vector<DiskEntry> files;
        std::size_t totalBytes = 0;
        QDirIterator iterator(
            config_.diskDirectory,
            QStringList{QStringLiteral("*.vsthumb")},
            QDir::Files,
            QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QFileInfo info(iterator.next());
            const std::size_t bytes = safeFileSize(info.size());
            totalBytes += bytes;
            files.push_back(DiskEntry{
                info.absoluteFilePath(),
                info.lastModified().toMSecsSinceEpoch(),
                bytes,
            });
        }

        if (totalBytes <= config_.diskBudgetBytes) {
            return;
        }

        std::sort(files.begin(), files.end(), [](const DiskEntry& left, const DiskEntry& right) {
            if (left.modifiedMilliseconds != right.modifiedMilliseconds) {
                return left.modifiedMilliseconds < right.modifiedMilliseconds;
            }
            return left.path < right.path;
        });

        for (const auto& entry : files) {
            if (totalBytes <= config_.diskBudgetBytes) {
                break;
            }
            if (QFile::remove(entry.path)) {
                totalBytes -= std::min(totalBytes, entry.bytes);
            }
        }
    }

    ThumbnailCacheConfig config_;
    mutable std::mutex memoryMutex_;
    EntryList entries_;
    std::unordered_map<ThumbnailCacheKey, EntryIterator, KeyHash> index_;
    std::size_t memoryBytes_ = 0;

    mutable std::mutex diskMutex_;
    std::uint64_t writesSincePrune_ = 0;

    std::atomic_uint64_t memoryHits_{0};
    std::atomic_uint64_t diskHits_{0};
    std::atomic_uint64_t misses_{0};
    std::atomic_uint64_t evictions_{0};
    std::atomic_uint64_t diskWrites_{0};
};

ThumbnailCache::ThumbnailCache(ThumbnailCacheConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

ThumbnailCache::~ThumbnailCache() = default;

QString ThumbnailCache::mediaIdentity(const media::MediaInfo& info)
{
    const QString sourcePath = pathToQString(info.path);
    QFileInfo fileInfo(sourcePath);
    QString canonicalPath = fileInfo.canonicalFilePath();
    if (canonicalPath.isEmpty()) {
        canonicalPath = fileInfo.absoluteFilePath();
    }
#ifdef Q_OS_WIN
    canonicalPath = canonicalPath.toCaseFolded();
#endif

    QByteArray identity;
    appendIdentityPart(identity, canonicalPath.toUtf8());
    appendIdentityPart(identity, QByteArray::number(fileInfo.size()));
    appendIdentityPart(identity, QByteArray::number(fileInfo.lastModified().toMSecsSinceEpoch()));
    appendIdentityPart(identity, QByteArray::number(info.videoStreamIndex));
    appendIdentityPart(identity, QByteArray::number(kDiskSchemaVersion));
    return QString::fromLatin1(stableHash(std::move(identity)));
}

std::optional<ThumbnailFrame> ThumbnailCache::lookupMemory(const ThumbnailCacheKey& key)
{
    return impl_->lookupMemory(key);
}

std::optional<ThumbnailFrame> ThumbnailCache::lookup(const ThumbnailCacheKey& key)
{
    return impl_->lookup(key);
}

std::optional<ThumbnailCacheLookup> ThumbnailCache::lookupWithSource(
    const ThumbnailCacheKey& key)
{
    return impl_->lookupWithSource(key);
}

void ThumbnailCache::insert(const ThumbnailCacheKey& key, const ThumbnailFrame& frame)
{
    impl_->insert(key, frame);
}

void ThumbnailCache::insertMemory(const ThumbnailCacheKey& key, const ThumbnailFrame& frame)
{
    impl_->insertMemory(key, frame);
}

void ThumbnailCache::insertDisk(const ThumbnailCacheKey& key, const ThumbnailFrame& frame)
{
    impl_->insertDisk(key, frame);
}

void ThumbnailCache::clearMemory()
{
    impl_->clearMemory();
}

void ThumbnailCache::clearMedia(const QString& mediaIdentity)
{
    impl_->clearMedia(mediaIdentity);
}

void ThumbnailCache::pruneDisk()
{
    impl_->pruneDisk();
}

ThumbnailCacheStats ThumbnailCache::stats() const
{
    return impl_->stats();
}

} // namespace vidscope::thumbnails
