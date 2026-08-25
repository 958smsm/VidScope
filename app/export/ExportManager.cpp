#include "export/ExportManager.h"

#include "core/Cancellation.h"
#include "media/FrameConverter.h"
#include "playback/PlaybackSession.h"
#include "playback/SeekController.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QSaveFile>
#include <QtGui/QFont>
#include <QtGui/QImageWriter>
#include <QtGui/QPainter>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

namespace vidscope::exporting {
namespace {

constexpr int kSheetMargin = 16;
constexpr int kSheetGap = 8;
constexpr int kSheetLabelHeight = 34;
constexpr std::size_t kMaximumReportedPaths = 1'024;

class ExportFailure final : public std::runtime_error {
public:
    explicit ExportFailure(const QString& message)
        : std::runtime_error(message.toUtf8().constData())
        , detail_(message)
    {
    }

    [[nodiscard]] const QString& detail() const noexcept
    {
        return detail_;
    }

private:
    QString detail_;
};

[[nodiscard]] QString pathToQString(const std::filesystem::path& path)
{
#if defined(_WIN32)
    return QString::fromStdWString(path.wstring());
#else
    return QString::fromStdString(path.string());
#endif
}

[[nodiscard]] std::filesystem::path qStringToPath(const QString& value)
{
#if defined(_WIN32)
    return std::filesystem::path(value.toStdWString());
#else
    return std::filesystem::path(value.toStdString());
#endif
}

[[nodiscard]] std::filesystem::path pathWithDefaultExtension(
    std::filesystem::path path,
    const ImageFormat format)
{
    if (!ExportPlanner::formatFromPath(path)) {
        path += qStringToPath(
            QStringLiteral(".%1").arg(ExportPlanner::extension(format)));
    }
    return path;
}

void ensureParentDirectory(const std::filesystem::path& path)
{
    const auto parent = path.parent_path();
    if (!parent.empty() && !QDir().mkpath(pathToQString(parent))) {
        throw ExportFailure(
            QStringLiteral("Could not create output directory: %1")
                .arg(pathToQString(parent)));
    }
}

void ensureSequenceDirectory(const std::filesystem::path& path)
{
    const QFileInfo info(pathToQString(path));
    if (info.exists() && !info.isDir()) {
        throw ExportFailure(
            QStringLiteral("The export destination is not a directory: %1")
                .arg(pathToQString(path)));
    }
    if (!QDir().mkpath(pathToQString(path))) {
        throw ExportFailure(
            QStringLiteral("Could not create export directory: %1")
                .arg(pathToQString(path)));
    }
}

void ensureFormatAvailable(const ImageFormat format)
{
    const QByteArray requested = ExportPlanner::formatName(format);
    const auto supported = QImageWriter::supportedImageFormats();
    if (std::none_of(
            supported.cbegin(),
            supported.cend(),
            [&](const QByteArray& value) {
                return value.compare(requested, Qt::CaseInsensitive) == 0
                    || (requested == QByteArrayLiteral("jpeg")
                        && value.compare(QByteArrayLiteral("jpg"), Qt::CaseInsensitive) == 0)
                    || (requested == QByteArrayLiteral("tiff")
                        && value.compare(QByteArrayLiteral("tif"), Qt::CaseInsensitive) == 0);
            })) {
        throw ExportFailure(
            QStringLiteral("The Qt image plugin for %1 is not available.")
                .arg(QString::fromLatin1(requested).toUpper()));
    }
}

void writeImageAtomically(
    const QImage& image,
    const std::filesystem::path& path,
    const ImageFormat format,
    const int quality,
    const bool overwrite)
{
    if (image.isNull()) {
        throw ExportFailure(QStringLiteral("The exported image is empty."));
    }
    ensureParentDirectory(path);
    const QString fileName = pathToQString(path);
    if (!overwrite && QFileInfo::exists(fileName)) {
        throw ExportFailure(
            QStringLiteral("Output already exists: %1").arg(fileName));
    }

    QSaveFile file(fileName);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        throw ExportFailure(
            QStringLiteral("Could not open output file: %1").arg(file.errorString()));
    }
    QImageWriter writer(&file, ExportPlanner::formatName(format));
    writer.setQuality(quality);
    writer.setOptimizedWrite(true);
    writer.setText(QStringLiteral("Software"), QStringLiteral("VidScope"));
    if (!writer.write(image)) {
        file.cancelWriting();
        throw ExportFailure(
            QStringLiteral("Could not encode %1: %2")
                .arg(fileName, writer.errorString()));
    }
    if (!file.commit()) {
        throw ExportFailure(
            QStringLiteral("Could not commit %1: %2")
                .arg(fileName, file.errorString()));
    }
}

[[nodiscard]] QString formattedTimestamp(const media::MediaTime time)
{
    const qint64 totalMilliseconds =
        std::max<qint64>(0, static_cast<qint64>(time.count() / 1'000'000));
    const qint64 hours = totalMilliseconds / 3'600'000;
    const qint64 minutes = (totalMilliseconds / 60'000) % 60;
    const qint64 seconds = (totalMilliseconds / 1'000) % 60;
    const qint64 milliseconds = totalMilliseconds % 1'000;
    return QStringLiteral("%1:%2:%3.%4")
        .arg(hours, 2, 10, QChar(u'0'))
        .arg(minutes, 2, 10, QChar(u'0'))
        .arg(seconds, 2, 10, QChar(u'0'))
        .arg(milliseconds, 3, 10, QChar(u'0'));
}

[[nodiscard]] quint64 estimatedRangeTotal(
    const ExportRequest& request,
    const media::MediaInfo& info) noexcept
{
    if (info.declaredFrameCount <= 0 || info.duration <= media::MediaTime::zero()) {
        return 0;
    }
    const auto span = std::max(
        media::MediaTime::zero(),
        request.rangeEnd - request.rangeStart);
    const long double fraction = std::clamp(
        static_cast<long double>(span.count())
            / static_cast<long double>(info.duration.count()),
        0.0L,
        1.0L);
    const long double estimated =
        static_cast<long double>(info.declaredFrameCount) * fraction
        / static_cast<long double>(request.everyNFrames);
    return static_cast<quint64>(std::min<long double>(
        std::ceil(estimated),
        static_cast<long double>(request.maximumFrames)));
}

[[nodiscard]] bool sameFrame(
    const media::DecodedFrame& left,
    const media::DecodedFrame& right) noexcept
{
    if (left.id.presentationIndex >= 0 && right.id.presentationIndex >= 0) {
        return left.id.presentationIndex == right.id.presentationIndex;
    }
    return left.presentationTime == right.presentationTime
        && left.id.pts == right.id.pts;
}

class ExportOperation final {
public:
    using Progress = std::function<void(quint64, quint64, QString)>;

    ExportOperation(
        media::MediaInfoPtr info,
        ExportRequest request,
        const core::CancellationToken cancellation,
        Progress progress)
        : info_(std::move(info))
        , request_(std::move(request))
        , cancellation_(cancellation)
        , progress_(std::move(progress))
        , session_(sessionConfig())
    {
    }

    [[nodiscard]] ExportSummary run(const quint64 generation)
    {
        ExportSummary summary;
        summary.generation = generation;
        summary.state = ExportState::Running;
        if (!info_) {
            return failed(std::move(summary), QStringLiteral("No media is available for export."));
        }
        if (request_.outputPath.empty()) {
            return failed(std::move(summary), QStringLiteral("No export destination was selected."));
        }

        try {
            ensureFormatAvailable(request_.format);
            const auto opened = session_.open(info_->path, cancellation_);
            if (cancelled(opened)) {
                return cancelledSummary(std::move(summary));
            }
            if (!opened) {
                return failed(std::move(summary), QStringLiteral("Could not decode the first video frame."));
            }
            switch (request_.kind) {
            case ExportKind::SingleFrame:
                exportSingle(opened.frame, summary);
                break;
            case ExportKind::FrameRange:
            case ExportKind::Keyframes:
                exportRange(opened.frame, summary);
                break;
            case ExportKind::SceneFrames:
            case ExportKind::HighMotionFrames:
                exportTargets(summary);
                break;
            case ExportKind::ContactSheet:
                exportContactSheet(summary);
                break;
            }
            if (isCancelled()) {
                return cancelledSummary(std::move(summary));
            }
            summary.state = ExportState::Completed;
            if (summary.filesWritten == 0 && summary.detail.isEmpty()) {
                summary.detail = QStringLiteral("No frames matched the export request.");
            }
            return summary;
        } catch (const ExportFailure& error) {
            return failed(std::move(summary), error.detail());
        } catch (const std::exception& error) {
            return failed(std::move(summary), QString::fromUtf8(error.what()));
        }
    }

private:
    [[nodiscard]] static playback::PlaybackSessionConfig sessionConfig()
    {
        playback::PlaybackSessionConfig config;
        config.frameCacheBytes = 64ULL * 1024ULL * 1024ULL;
        config.forwardQueueBytes = 32ULL * 1024ULL * 1024ULL;
        config.forwardQueueFrames = 4;
        config.initialPrefetchFrames = 1;
        config.presentationIndexAnchorCount = 8'192;
        config.decoder.hardwareAcceleration = media::HardwareAcceleration::Disabled;
        return config;
    }

    [[nodiscard]] bool isCancelled() const noexcept
    {
        return cancellation_.isCancellationRequested();
    }

    [[nodiscard]] static bool cancelled(
        const playback::NavigationResult& result) noexcept
    {
        return result.status == playback::NavigationStatus::Cancelled;
    }

    [[nodiscard]] ExportSummary failed(
        ExportSummary summary,
        QString detail) const
    {
        summary.state = isCancelled() ? ExportState::Cancelled : ExportState::Failed;
        summary.detail = std::move(detail);
        return summary;
    }

    [[nodiscard]] ExportSummary cancelledSummary(ExportSummary summary) const
    {
        summary.state = ExportState::Cancelled;
        summary.detail = QStringLiteral("Export cancelled.");
        return summary;
    }

    void noteFile(ExportSummary& summary, const std::filesystem::path& path)
    {
        ++summary.filesWritten;
        if (summary.outputFiles.size()
            < static_cast<qsizetype>(kMaximumReportedPaths)) {
            summary.outputFiles.push_back(pathToQString(path));
        }
    }

    [[nodiscard]] QImage convert(
        const media::DecodedFramePtr& frame,
        ExportSummary& summary)
    {
        if (!frame || isCancelled()) {
            return {};
        }
        ++summary.framesDecoded;
        return converter_.toBgraImage(*frame, cancellation_);
    }

    [[nodiscard]] media::DecodedFramePtr frameAt(
        const media::MediaTime time,
        const playback::SeekBias bias = playback::SeekBias::Nearest)
    {
        const auto result = session_.seek(
            playback::SeekRequest{0, time, bias},
            cancellation_);
        if (cancelled(result)) {
            return {};
        }
        return result ? result.frame : media::DecodedFramePtr{};
    }

    [[nodiscard]] media::DecodedFramePtr rangeStartFrame(
        const media::DecodedFramePtr& opened)
    {
        if (request_.rangeStart <= media::MediaTime::zero()) {
            return opened;
        }
        return frameAt(request_.rangeStart, playback::SeekBias::AtOrAfter);
    }

    [[nodiscard]] std::filesystem::path sequencePath(
        const media::DecodedFrame& frame,
        const quint64 sequence) const
    {
        return request_.outputPath
            / qStringToPath(
                ExportPlanner::frameFileName(
                    request_.baseName,
                    sequence,
                    frame,
                    request_.format));
    }

    void writeFrame(
        const media::DecodedFramePtr& frame,
        ExportSummary& summary,
        const std::filesystem::path& path)
    {
        const QImage image = convert(frame, summary);
        if (image.isNull()) {
            return;
        }
        writeImageAtomically(
            image,
            path,
            request_.format,
            request_.quality,
            request_.overwrite);
        noteFile(summary, path);
    }

    void exportSingle(
        const media::DecodedFramePtr& opened,
        ExportSummary& summary)
    {
        media::DecodedFramePtr frame = request_.anchor <= media::MediaTime::zero()
            ? opened
            : frameAt(request_.anchor);
        if (!frame || isCancelled()) {
            throw ExportFailure(QStringLiteral("Could not locate the requested frame."));
        }
        if (request_.relativeFrame == RelativeFrame::Previous) {
            const auto previous = session_.previousFrame(cancellation_);
            if (!previous) {
                throw ExportFailure(QStringLiteral("There is no previous frame to export."));
            }
            frame = previous.frame;
        } else if (request_.relativeFrame == RelativeFrame::Next) {
            const auto next = session_.nextFrame(cancellation_);
            if (!next) {
                throw ExportFailure(QStringLiteral("There is no next frame to export."));
            }
            frame = next.frame;
        }
        const auto path = pathWithDefaultExtension(request_.outputPath, request_.format);
        writeFrame(frame, summary, path);
        progress_(summary.filesWritten, 1, QStringLiteral("Saved frame"));
    }

    void exportRange(
        const media::DecodedFramePtr& opened,
        ExportSummary& summary)
    {
        ensureSequenceDirectory(request_.outputPath);
        media::DecodedFramePtr frame = rangeStartFrame(opened);
        if (!frame) {
            throw ExportFailure(QStringLiteral("Could not locate the start of the export range."));
        }
        const quint64 total = request_.kind == ExportKind::FrameRange
            ? estimatedRangeTotal(request_, *info_)
            : 0;
        std::size_t candidateIndex = 0;
        bool reachedLimit = false;
        for (;;) {
            if (isCancelled()) {
                return;
            }
            if (frame->presentationTime > request_.rangeEnd) {
                break;
            }
            const bool selected = request_.kind == ExportKind::Keyframes
                ? frame->keyFrame
                : candidateIndex % request_.everyNFrames == 0;
            if (selected) {
                if (summary.filesWritten >= request_.maximumFrames) {
                    reachedLimit = true;
                    break;
                }
                const auto path = sequencePath(*frame, summary.filesWritten + 1);
                writeFrame(frame, summary, path);
                progress_(
                    summary.filesWritten,
                    total,
                    QStringLiteral("Exported %1 frame(s)").arg(summary.filesWritten));
            }
            ++candidateIndex;
            const auto next = session_.nextFrame(cancellation_);
            if (cancelled(next) || !next) {
                break;
            }
            frame = next.frame;
        }
        if (reachedLimit) {
            summary.detail = QStringLiteral(
                "Export stopped at the configured %1-frame safety limit.")
                .arg(request_.maximumFrames);
        }
    }

    void exportTargets(ExportSummary& summary)
    {
        if (request_.targetTimes.empty()) {
            throw ExportFailure(QStringLiteral("No matching frames are available to export."));
        }
        ensureSequenceDirectory(request_.outputPath);
        media::DecodedFramePtr previous;
        const quint64 total = static_cast<quint64>(request_.targetTimes.size());
        for (const auto target : request_.targetTimes) {
            if (isCancelled()) {
                return;
            }
            auto frame = frameAt(target);
            if (!frame) {
                continue;
            }
            if (previous && sameFrame(*previous, *frame)) {
                continue;
            }
            const auto path = sequencePath(*frame, summary.filesWritten + 1);
            writeFrame(frame, summary, path);
            previous = std::move(frame);
            progress_(
                summary.filesWritten,
                total,
                QStringLiteral("Exported %1 of %2 frame(s)")
                    .arg(summary.filesWritten)
                    .arg(total));
        }
    }

    [[nodiscard]] std::vector<media::MediaTime> contactTargets() const
    {
        const std::size_t wanted =
            static_cast<std::size_t>(request_.contactSheet.frameCount);
        if (request_.targetTimes.empty()) {
            return ExportPlanner::evenlySpacedTimes(
                request_.rangeStart,
                request_.rangeEnd,
                wanted);
        }
        if (request_.targetTimes.size() <= wanted) {
            return request_.targetTimes;
        }
        const auto positions = ExportPlanner::evenlySpacedTimes(
            media::MediaTime::zero(),
            media::MediaTime(
                static_cast<media::MediaTime::rep>(request_.targetTimes.size() - 1)),
            wanted);
        std::vector<media::MediaTime> selected;
        selected.reserve(wanted);
        for (const auto position : positions) {
            const auto index = std::min<std::size_t>(
                request_.targetTimes.size() - 1,
                static_cast<std::size_t>(position.count()));
            selected.push_back(request_.targetTimes[index]);
        }
        return selected;
    }

    void exportContactSheet(ExportSummary& summary)
    {
        const QSize sheetSize =
            ExportPlanner::contactSheetPixelSize(request_.contactSheet);
        if (!sheetSize.isValid()) {
            throw ExportFailure(
                QStringLiteral("The requested contact sheet dimensions exceed the safety limit."));
        }
        const auto targets = contactTargets();
        if (targets.empty()) {
            throw ExportFailure(QStringLiteral("No frames are available for the contact sheet."));
        }

        QImage sheet(sheetSize, QImage::Format_RGB32);
        if (sheet.isNull()) {
            throw ExportFailure(QStringLiteral("Could not allocate the contact sheet."));
        }
        sheet.fill(QColor(18, 21, 27));
        QPainter painter(&sheet);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setPen(QColor(226, 231, 239));
        QFont labelFont = painter.font();
        labelFont.setPointSize(9);
        painter.setFont(labelFont);

        const bool hasLabels =
            request_.contactSheet.includeTimestamp
            || request_.contactSheet.includeFrameIndex;
        const int labelHeight = hasLabels ? kSheetLabelHeight : 0;
        const quint64 total = static_cast<quint64>(targets.size());
        media::DecodedFramePtr previous;
        int painted = 0;
        for (const auto target : targets) {
            if (isCancelled()) {
                painter.end();
                return;
            }
            auto frame = frameAt(target);
            if (!frame || (previous && sameFrame(*previous, *frame))) {
                continue;
            }
            const QImage fullImage = convert(frame, summary);
            if (fullImage.isNull()) {
                continue;
            }
            const int row = painted / request_.contactSheet.columns;
            const int column = painted % request_.contactSheet.columns;
            if (row >= request_.contactSheet.rows) {
                break;
            }
            const int left =
                kSheetMargin
                + column * (request_.contactSheet.cellSize.width() + kSheetGap);
            const int top =
                kSheetMargin
                + row * (
                    request_.contactSheet.cellSize.height()
                    + labelHeight
                    + kSheetGap);
            const QRect imageCell(
                left,
                top,
                request_.contactSheet.cellSize.width(),
                request_.contactSheet.cellSize.height());
            painter.fillRect(imageCell, QColor(5, 7, 10));
            const QSize scaled = fullImage.size().scaled(
                imageCell.size(),
                Qt::KeepAspectRatio);
            const QRect destination(
                imageCell.left() + (imageCell.width() - scaled.width()) / 2,
                imageCell.top() + (imageCell.height() - scaled.height()) / 2,
                scaled.width(),
                scaled.height());
            painter.drawImage(destination, fullImage);
            painter.setPen(QColor(75, 82, 94));
            painter.drawRect(imageCell.adjusted(0, 0, -1, -1));

            if (hasLabels) {
                QStringList parts;
                if (request_.contactSheet.includeTimestamp) {
                    parts.push_back(formattedTimestamp(frame->presentationTime));
                }
                if (request_.contactSheet.includeFrameIndex) {
                    parts.push_back(
                        frame->id.presentationIndex >= 0
                            ? QStringLiteral("Frame %1").arg(frame->id.presentationIndex)
                            : QStringLiteral("Frame ?"));
                }
                painter.setPen(QColor(226, 231, 239));
                painter.drawText(
                    QRect(
                        left,
                        imageCell.bottom() + 1,
                        imageCell.width(),
                        labelHeight),
                    Qt::AlignCenter | Qt::TextSingleLine,
                    parts.join(QStringLiteral("  |  ")));
            }
            previous = std::move(frame);
            ++painted;
            progress_(
                static_cast<quint64>(painted),
                total,
                QStringLiteral("Rendered %1 of %2 contact-sheet cells")
                    .arg(painted)
                    .arg(total));
        }
        painter.end();
        if (painted == 0) {
            throw ExportFailure(QStringLiteral("No distinct frames could be decoded."));
        }
        const auto path = pathWithDefaultExtension(request_.outputPath, request_.format);
        writeImageAtomically(
            sheet,
            path,
            request_.format,
            request_.quality,
            request_.overwrite);
        noteFile(summary, path);
    }

    media::MediaInfoPtr info_;
    ExportRequest request_;
    core::CancellationToken cancellation_;
    Progress progress_;
    playback::PlaybackSession session_;
    media::FrameConverter converter_;
};

} // namespace

class ExportManager::Impl final {
public:
    struct Task final {
        quint64 generation = 0;
        media::MediaInfoPtr info;
        ExportRequest request;
    };

    explicit Impl(ExportManager* owner)
        : owner_(owner)
        , worker_([this](const std::stop_token stop) { run(stop); })
    {
    }

    ~Impl()
    {
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
            cancellation_.requestCancellation();
            pending_.reset();
        }
        worker_.request_stop();
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void setMedia(media::MediaInfoPtr info)
    {
        std::lock_guard lock(mutex_);
        nextGeneration();
        cancellation_.requestCancellation();
        pending_.reset();
        if (!activeGeneration_) {
            busy_ = false;
        }
        info_ = std::move(info);
        state_ = ExportState::Idle;
    }

    [[nodiscard]] std::optional<quint64> start(ExportRequest request)
    {
        std::lock_guard lock(mutex_);
        if (closing_ || busy_ || !info_) {
            return std::nullopt;
        }
        nextGeneration();
        cancellation_ = core::CancellationSource{};
        request = ExportPlanner::normalized(std::move(request), info_->duration);
        pending_ = Task{generation_, info_, std::move(request)};
        busy_ = true;
        state_ = ExportState::Running;
        condition_.notify_all();
        return generation_;
    }

    [[nodiscard]] bool cancel()
    {
        std::lock_guard lock(mutex_);
        if (!busy_) {
            return false;
        }
        state_ = ExportState::Cancelling;
        cancellation_.requestCancellation();
        condition_.notify_all();
        return true;
    }

    [[nodiscard]] bool busy() const noexcept
    {
        std::lock_guard lock(mutex_);
        return busy_;
    }

    [[nodiscard]] ExportState state() const noexcept
    {
        std::lock_guard lock(mutex_);
        return state_;
    }

    [[nodiscard]] bool accepts(const quint64 generation) const noexcept
    {
        std::lock_guard lock(mutex_);
        return !closing_ && generation_ == generation;
    }

private:
    void nextGeneration() noexcept
    {
        generation_ = generation_ == std::numeric_limits<quint64>::max()
            ? 1
            : generation_ + 1;
    }

    void postProgress(
        const quint64 generation,
        const quint64 completed,
        const quint64 total,
        QString detail)
    {
        QPointer<ExportManager> guard(owner_);
        QMetaObject::invokeMethod(
            owner_,
            [guard,
             generation,
             completed,
             total,
             detail = std::move(detail)]() mutable {
                if (guard) {
                    guard->deliverProgress(
                        generation,
                        completed,
                        total,
                        std::move(detail));
                }
            },
            Qt::QueuedConnection);
    }

    void postFinished(ExportSummary summary)
    {
        QPointer<ExportManager> guard(owner_);
        QMetaObject::invokeMethod(
            owner_,
            [guard, summary = std::move(summary)]() mutable {
                if (guard) {
                    guard->deliverFinished(std::move(summary));
                }
            },
            Qt::QueuedConnection);
    }

    void postState(const quint64 generation, const ExportState state)
    {
        QPointer<ExportManager> guard(owner_);
        QMetaObject::invokeMethod(
            owner_,
            [guard, generation, state] {
                if (guard) {
                    guard->deliverState(generation, state);
                }
            },
            Qt::QueuedConnection);
    }

    void run(const std::stop_token stop)
    {
        while (!stop.stop_requested()) {
            Task task;
            core::CancellationToken cancellation;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, stop, [this] {
                    return closing_ || pending_.has_value();
                });
                if (closing_ || stop.stop_requested()) {
                    return;
                }
                task = std::move(*pending_);
                pending_.reset();
                activeGeneration_ = task.generation;
                cancellation = cancellation_.token();
            }

            ExportOperation operation(
                task.info,
                std::move(task.request),
                cancellation,
                [this, generation = task.generation](
                    const quint64 completed,
                    const quint64 total,
                    QString detail) {
                    if (accepts(generation)) {
                        postProgress(
                            generation,
                            completed,
                            total,
                            std::move(detail));
                    }
                });
            ExportSummary summary = operation.run(task.generation);
            bool deliver = false;
            std::optional<quint64> idleGeneration;
            {
                std::lock_guard lock(mutex_);
                activeGeneration_.reset();
                busy_ = false;
                if (!closing_ && generation_ == task.generation) {
                    state_ = summary.state;
                    deliver = true;
                } else if (!closing_) {
                    state_ = ExportState::Idle;
                    idleGeneration = generation_;
                }
            }
            if (deliver) {
                postFinished(std::move(summary));
            } else if (idleGeneration) {
                postState(*idleGeneration, ExportState::Idle);
            }
        }
    }

    ExportManager* const owner_;
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    media::MediaInfoPtr info_;
    std::optional<Task> pending_;
    std::optional<quint64> activeGeneration_;
    core::CancellationSource cancellation_;
    quint64 generation_ = 0;
    ExportState state_ = ExportState::Idle;
    bool busy_ = false;
    bool closing_ = false;
    std::jthread worker_;
};

ExportManager::ExportManager(QObject* parent)
    : QObject(parent)
    , impl_(std::make_unique<Impl>(this))
{
    setObjectName(QStringLiteral("exportManager"));
}

ExportManager::~ExportManager() = default;

void ExportManager::setMedia(media::MediaInfoPtr info)
{
    impl_->setMedia(std::move(info));
    emit stateChanged(ExportState::Idle);
}

void ExportManager::clearMedia()
{
    setMedia({});
}

bool ExportManager::startExport(ExportRequest request)
{
    const auto generation = impl_->start(std::move(request));
    if (!generation) {
        return false;
    }
    emit stateChanged(ExportState::Running);
    return true;
}

void ExportManager::cancel()
{
    if (impl_->cancel()) {
        emit stateChanged(ExportState::Cancelling);
    }
}

bool ExportManager::isBusy() const noexcept
{
    return impl_->busy();
}

ExportState ExportManager::state() const noexcept
{
    return impl_->state();
}

void ExportManager::deliverProgress(
    const quint64 generation,
    const quint64 completed,
    const quint64 total,
    QString detail)
{
    if (impl_->accepts(generation)) {
        emit progressChanged(
            generation,
            completed,
            total,
            std::move(detail));
    }
}

void ExportManager::deliverState(
    const quint64 generation,
    const ExportState state)
{
    if (impl_->accepts(generation)) {
        emit stateChanged(state);
    }
}

void ExportManager::deliverFinished(ExportSummary summary)
{
    if (impl_->accepts(summary.generation)) {
        emit stateChanged(summary.state);
        emit exportFinished(summary);
    }
}

} // namespace vidscope::exporting
