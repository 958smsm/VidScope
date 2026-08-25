#include "analysis/AnalysisCache.h"

#include "core/Logging.h"

#include <QtCore/QByteArray>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDataStream>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QDirIterator>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QStringList>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace vidscope::analysis {
namespace {

constexpr quint32 kDiskMagic = 0x5653414EU; // "VSAN"
constexpr quint16 kDiskSchemaVersion = 1U;
constexpr quint16 kAlgorithmVersion = 1U;

[[nodiscard]] QString pathToQString(const std::filesystem::path& path)
{
#ifdef Q_OS_WIN
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

void appendIdentityPart(QByteArray& destination, const QByteArray& part)
{
    destination.append(QByteArray::number(static_cast<qlonglong>(part.size())));
    destination.append(':');
    destination.append(part);
    destination.append('|');
}

[[nodiscard]] QString stableHash(QByteArray input)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(std::move(input), QCryptographicHash::Sha256).toHex());
}

[[nodiscard]] bool validScore(const std::optional<float>& value) noexcept
{
    return !value || (std::isfinite(*value) && *value >= 0.0F && *value <= 1.0F);
}

[[nodiscard]] std::size_t safeFileSize(const qint64 value) noexcept
{
    if (value <= 0) {
        return 0;
    }
    const auto converted = static_cast<quint64>(value);
    return static_cast<std::size_t>(std::min(
        converted,
        static_cast<quint64>(std::numeric_limits<std::size_t>::max())));
}

} // namespace

class AnalysisCache::Impl final {
public:
    explicit Impl(AnalysisCacheConfig config)
        : config_(std::move(config))
    {
        if (!config_.diskDirectory.isEmpty() && config_.diskBudgetBytes > 0) {
            (void)QDir().mkpath(config_.diskDirectory);
            pruneLocked();
        }
    }

    [[nodiscard]] AnalysisCacheDocument load(const media::MediaInfo& info) const
    {
        std::lock_guard lock(mutex_);
        AnalysisCacheDocument document;
        const QString identity = AnalysisCache::mediaIdentity(info);
        const QString path = filePath(identity);
        if (path.isEmpty()) {
            return document;
        }

        const QFileInfo fileInfo(path);
        if (!fileInfo.exists() || fileInfo.size() <= 0
            || safeFileSize(fileInfo.size()) > config_.maximumDocumentBytes) {
            return document;
        }

        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return document;
        }
        QDataStream stream(&file);
        stream.setVersion(QDataStream::Qt_6_0);

        quint32 magic = 0;
        quint16 schema = 0;
        quint16 algorithm = 0;
        QString storedIdentity;
        bool complete = false;
        quint64 count = 0;
        stream >> magic >> schema >> algorithm >> storedIdentity >> complete >> count;
        if (magic != kDiskMagic || schema != kDiskSchemaVersion
            || algorithm != kAlgorithmVersion || storedIdentity != identity
            || count > static_cast<quint64>(config_.maximumSamples)) {
            file.close();
            qCWarning(logCache) << "Discarding incompatible analysis cache" << path;
            (void)QFile::remove(path);
            return {};
        }

        document.samples.reserve(static_cast<std::size_t>(count));
        for (quint64 index = 0; index < count && stream.status() == QDataStream::Ok; ++index) {
            AnalysisSample sample;
            qint64 time = 0;
            qint64 duration = 0;
            qint64 presentationIndex = -1;
            qint64 pts = AV_NOPTS_VALUE;
            bool keyFrame = false;
            bool hasMotion = false;
            float motion = 0.0F;
            bool hasSimilarity = false;
            float similarity = 0.0F;
            stream >> time >> duration >> presentationIndex >> pts >> keyFrame
                >> hasMotion >> motion >> hasSimilarity >> similarity;
            sample.presentationTime = media::MediaTime(time);
            sample.duration = media::MediaTime(duration);
            sample.presentationIndex = presentationIndex;
            sample.pts = pts;
            sample.keyFrame = keyFrame;
            if (hasMotion) {
                sample.motion = motion;
            }
            if (hasSimilarity) {
                sample.similarity = similarity;
            }
            if (sample.presentationTime < media::MediaTime::zero()
                || sample.duration < media::MediaTime::zero()
                || !validScore(sample.motion) || !validScore(sample.similarity)) {
                stream.setStatus(QDataStream::ReadCorruptData);
                break;
            }
            document.samples.push_back(std::move(sample));
        }

        const bool valid = stream.status() == QDataStream::Ok
            && document.samples.size() == static_cast<std::size_t>(count);
        if (valid) {
            document.complete = complete;
            (void)file.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileModificationTime);
            return document;
        }

        file.close();
        qCWarning(logCache) << "Discarding corrupt analysis cache" << path;
        (void)QFile::remove(path);
        return {};
    }

    [[nodiscard]] bool save(
        const media::MediaInfo& info,
        const std::vector<AnalysisSample>& samples,
        const bool complete)
    {
        std::lock_guard lock(mutex_);
        if (config_.diskBudgetBytes == 0 || config_.maximumDocumentBytes == 0
            || config_.diskDirectory.isEmpty() || samples.size() > config_.maximumSamples) {
            return false;
        }

        constexpr std::size_t approximateSerializedSampleBytes = 48;
        if (samples.size() > config_.maximumDocumentBytes / approximateSerializedSampleBytes) {
            qCWarning(logCache) << "Analysis cache document exceeds its configured bound";
            return false;
        }

        const QString identity = AnalysisCache::mediaIdentity(info);
        const QString path = filePath(identity);
        if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) {
            return false;
        }

        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            return false;
        }
        QDataStream stream(&file);
        stream.setVersion(QDataStream::Qt_6_0);
        stream << kDiskMagic << kDiskSchemaVersion << kAlgorithmVersion
               << identity << complete << static_cast<quint64>(samples.size());

        for (const auto& sample : samples) {
            if (sample.presentationTime < media::MediaTime::zero()
                || sample.duration < media::MediaTime::zero()
                || !validScore(sample.motion) || !validScore(sample.similarity)) {
                file.cancelWriting();
                return false;
            }
            stream << static_cast<qint64>(sample.presentationTime.count())
                   << static_cast<qint64>(sample.duration.count())
                   << static_cast<qint64>(sample.presentationIndex)
                   << static_cast<qint64>(sample.pts)
                   << sample.keyFrame
                   << sample.motion.has_value() << sample.motion.value_or(0.0F)
                   << sample.similarity.has_value() << sample.similarity.value_or(0.0F);
        }

        if (stream.status() != QDataStream::Ok
            || static_cast<std::size_t>(std::max<qint64>(0, file.size()))
                > config_.maximumDocumentBytes
            || !file.commit()) {
            return false;
        }
        pruneLocked();
        return true;
    }

    void clearMedia(const media::MediaInfo& info)
    {
        std::lock_guard lock(mutex_);
        const QString path = filePath(AnalysisCache::mediaIdentity(info));
        if (!path.isEmpty()) {
            (void)QFile::remove(path);
        }
    }

    void prune()
    {
        std::lock_guard lock(mutex_);
        pruneLocked();
    }

private:
    [[nodiscard]] QString filePath(const QString& identity) const
    {
        if (config_.diskDirectory.isEmpty() || identity.isEmpty()) {
            return {};
        }
        return QDir(config_.diskDirectory).filePath(identity + QStringLiteral(".vsanalysis"));
    }

    void pruneLocked() const
    {
        if (config_.diskDirectory.isEmpty() || config_.diskBudgetBytes == 0) {
            return;
        }
        struct Entry final {
            QString path;
            qint64 modified = 0;
            std::size_t bytes = 0;
        };
        std::vector<Entry> entries;
        std::size_t totalBytes = 0;
        QDirIterator iterator(
            config_.diskDirectory,
            QStringList{QStringLiteral("*.vsanalysis")},
            QDir::Files);
        while (iterator.hasNext()) {
            const QFileInfo info(iterator.next());
            const std::size_t bytes = safeFileSize(info.size());
            totalBytes += bytes;
            entries.push_back({info.absoluteFilePath(), info.lastModified().toMSecsSinceEpoch(), bytes});
        }
        if (totalBytes <= config_.diskBudgetBytes) {
            return;
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
            if (left.modified != right.modified) {
                return left.modified < right.modified;
            }
            return left.path < right.path;
        });
        for (const auto& entry : entries) {
            if (totalBytes <= config_.diskBudgetBytes) {
                break;
            }
            if (QFile::remove(entry.path)) {
                totalBytes -= std::min(totalBytes, entry.bytes);
            }
        }
    }

    AnalysisCacheConfig config_;
    mutable std::mutex mutex_;
};

AnalysisCache::AnalysisCache(AnalysisCacheConfig config)
    : impl_(std::make_unique<Impl>(std::move(config)))
{
}

AnalysisCache::~AnalysisCache() = default;

QString AnalysisCache::mediaIdentity(const media::MediaInfo& info)
{
    QFileInfo file(pathToQString(info.path));
    QString canonicalPath = file.canonicalFilePath();
    if (canonicalPath.isEmpty()) {
        canonicalPath = file.absoluteFilePath();
    }
#ifdef Q_OS_WIN
    canonicalPath = canonicalPath.toCaseFolded();
#endif
    QByteArray identity;
    appendIdentityPart(identity, canonicalPath.toUtf8());
    appendIdentityPart(identity, QByteArray::number(file.size()));
    appendIdentityPart(identity, QByteArray::number(file.lastModified().toMSecsSinceEpoch()));
    appendIdentityPart(identity, QByteArray::number(info.videoStreamIndex));
    appendIdentityPart(identity, QByteArray::number(kDiskSchemaVersion));
    appendIdentityPart(identity, QByteArray::number(kAlgorithmVersion));
    return stableHash(std::move(identity));
}

AnalysisCacheDocument AnalysisCache::load(const media::MediaInfo& info) const
{
    return impl_->load(info);
}

bool AnalysisCache::save(
    const media::MediaInfo& info,
    const std::vector<AnalysisSample>& samples,
    const bool complete)
{
    return impl_->save(info, samples, complete);
}

void AnalysisCache::clearMedia(const media::MediaInfo& info)
{
    impl_->clearMedia(info);
}

void AnalysisCache::prune()
{
    impl_->prune();
}

} // namespace vidscope::analysis

