#pragma once

#include "media/MediaTypes.h"

#include <QtCore/QByteArray>
#include <QtCore/QMetaType>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace vidscope::exporting {

enum class ExportKind : std::uint8_t {
    SingleFrame,
    FrameRange,
    Keyframes,
    SceneFrames,
    HighMotionFrames,
    ContactSheet,
};

enum class RelativeFrame : std::int8_t {
    Previous = -1,
    Current = 0,
    Next = 1,
};

enum class ImageFormat : std::uint8_t {
    Png,
    Jpeg,
    WebP,
    Bmp,
    Tiff,
};

enum class ContactSheetSource : std::uint8_t {
    EntireVideo,
    VisibleRange,
    SelectedRange,
    DetectedScenes,
};

enum class ExportState : std::uint8_t {
    Idle,
    Running,
    Cancelling,
    Completed,
    Cancelled,
    Failed,
};

struct ContactSheetOptions final {
    ContactSheetSource source = ContactSheetSource::EntireVideo;
    int rows = 4;
    int columns = 5;
    int frameCount = 20;
    QSize cellSize{320, 180};
    bool includeTimestamp = true;
    bool includeFrameIndex = true;

    friend bool operator==(const ContactSheetOptions&, const ContactSheetOptions&) = default;
};

struct ExportRequest final {
    ExportKind kind = ExportKind::SingleFrame;
    RelativeFrame relativeFrame = RelativeFrame::Current;
    ImageFormat format = ImageFormat::Png;
    std::filesystem::path outputPath;
    QString baseName;
    media::MediaTime anchor{};
    media::MediaTime rangeStart{};
    media::MediaTime rangeEnd{};
    std::size_t everyNFrames = 1;
    std::size_t maximumFrames = 100'000;
    int quality = 92;
    bool overwrite = false;
    std::vector<media::MediaTime> targetTimes;
    ContactSheetOptions contactSheet;
};

struct ExportSummary final {
    ExportState state = ExportState::Idle;
    quint64 generation = 0;
    quint64 framesDecoded = 0;
    quint64 filesWritten = 0;
    QStringList outputFiles;
    QString detail;
};

class ExportPlanner final {
public:
    static constexpr std::size_t kMaximumFrameExports = 100'000;
    static constexpr int kMaximumContactSheetCells = 1'024;
    static constexpr qint64 kMaximumContactSheetPixels = 64LL * 1024LL * 1024LL;

    [[nodiscard]] static ExportRequest normalized(
        ExportRequest request,
        media::MediaTime mediaDuration);
    [[nodiscard]] static std::vector<media::MediaTime> evenlySpacedTimes(
        media::MediaTime start,
        media::MediaTime end,
        std::size_t count);
    [[nodiscard]] static QSize presetGrid(int frameCount) noexcept;
    [[nodiscard]] static QSize contactSheetPixelSize(
        const ContactSheetOptions& options) noexcept;
    [[nodiscard]] static QString sanitizedBaseName(QString name);
    [[nodiscard]] static QString frameFileName(
        const QString& baseName,
        quint64 sequence,
        const media::DecodedFrame& frame,
        ImageFormat format);
    [[nodiscard]] static QByteArray formatName(ImageFormat format);
    [[nodiscard]] static QString extension(ImageFormat format);
    [[nodiscard]] static QString fileDialogFilter(
        bool includeTiff = true,
        bool includeBmp = true);
    [[nodiscard]] static std::optional<ImageFormat> formatFromPath(
        const std::filesystem::path& path) noexcept;
};

} // namespace vidscope::exporting

Q_DECLARE_METATYPE(vidscope::exporting::ExportState)
Q_DECLARE_METATYPE(vidscope::exporting::ExportSummary)
