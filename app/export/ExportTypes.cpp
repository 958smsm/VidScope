#include "export/ExportTypes.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vidscope::exporting {
namespace {

constexpr int kContactSheetMargin = 16;
constexpr int kContactSheetGap = 8;
constexpr int kContactSheetLabelHeight = 34;

[[nodiscard]] media::MediaTime clampTime(
    media::MediaTime value,
    const media::MediaTime duration) noexcept
{
    value = std::max(value, media::MediaTime::zero());
    if (duration > media::MediaTime::zero()) {
        value = std::min(value, duration);
    }
    return value;
}

} // namespace

ExportRequest ExportPlanner::normalized(
    ExportRequest request,
    const media::MediaTime mediaDuration)
{
    request.anchor = clampTime(request.anchor, mediaDuration);
    request.rangeStart = clampTime(request.rangeStart, mediaDuration);
    request.rangeEnd = clampTime(request.rangeEnd, mediaDuration);
    if (request.rangeEnd < request.rangeStart) {
        std::swap(request.rangeStart, request.rangeEnd);
    }
    if (request.rangeEnd == request.rangeStart && mediaDuration > request.rangeStart) {
        request.rangeEnd = mediaDuration;
    }
    request.everyNFrames = std::clamp<std::size_t>(
        request.everyNFrames,
        1,
        1'000'000);
    request.maximumFrames = std::clamp<std::size_t>(
        request.maximumFrames,
        1,
        kMaximumFrameExports);
    request.quality = std::clamp(request.quality, 0, 100);
    request.baseName = sanitizedBaseName(std::move(request.baseName));

    auto& sheet = request.contactSheet;
    sheet.rows = std::clamp(sheet.rows, 1, 32);
    sheet.columns = std::clamp(sheet.columns, 1, 32);
    const int capacity = std::min(
        kMaximumContactSheetCells,
        sheet.rows * sheet.columns);
    sheet.frameCount = std::clamp(sheet.frameCount, 1, capacity);
    sheet.cellSize.setWidth(std::clamp(sheet.cellSize.width(), 64, 1'920));
    sheet.cellSize.setHeight(std::clamp(sheet.cellSize.height(), 48, 1'080));

    for (auto& time : request.targetTimes) {
        time = clampTime(time, mediaDuration);
    }
    std::sort(request.targetTimes.begin(), request.targetTimes.end());
    request.targetTimes.erase(
        std::unique(request.targetTimes.begin(), request.targetTimes.end()),
        request.targetTimes.end());
    if (request.targetTimes.size() > request.maximumFrames) {
        request.targetTimes.resize(request.maximumFrames);
    }
    return request;
}

std::vector<media::MediaTime> ExportPlanner::evenlySpacedTimes(
    const media::MediaTime start,
    const media::MediaTime end,
    const std::size_t count)
{
    if (count == 0) {
        return {};
    }
    if (count == 1 || end <= start) {
        return {start};
    }

    std::vector<media::MediaTime> result;
    result.reserve(count);
    const auto span = (end - start).count();
    const auto intervals = static_cast<std::int64_t>(count - 1);
    const auto quotient = span / intervals;
    const auto remainder = span % intervals;
    for (std::size_t index = 0; index < count; ++index) {
        const auto position = static_cast<std::int64_t>(index);
        const auto offset =
            quotient * position + (remainder * position) / intervals;
        result.emplace_back(start.count() + offset);
    }
    return result;
}

QSize ExportPlanner::presetGrid(const int frameCount) noexcept
{
    switch (frameCount) {
    case 8:
        return {4, 2};
    case 16:
        return {4, 4};
    case 20:
        return {5, 4};
    case 25:
        return {5, 5};
    default: {
        const int columns = std::max(
            1,
            static_cast<int>(std::ceil(std::sqrt(
                static_cast<double>(std::max(1, frameCount))))));
        const int rows = std::max(1, (std::max(1, frameCount) + columns - 1) / columns);
        return {columns, rows};
    }
    }
}

QSize ExportPlanner::contactSheetPixelSize(
    const ContactSheetOptions& options) noexcept
{
    if (options.rows <= 0 || options.columns <= 0
        || options.cellSize.width() <= 0 || options.cellSize.height() <= 0) {
        return {};
    }
    const qint64 labelHeight =
        options.includeTimestamp || options.includeFrameIndex
        ? kContactSheetLabelHeight
        : 0;
    const qint64 width =
        2LL * kContactSheetMargin
        + static_cast<qint64>(options.columns) * options.cellSize.width()
        + static_cast<qint64>(options.columns - 1) * kContactSheetGap;
    const qint64 height =
        2LL * kContactSheetMargin
        + static_cast<qint64>(options.rows)
            * (static_cast<qint64>(options.cellSize.height()) + labelHeight)
        + static_cast<qint64>(options.rows - 1) * kContactSheetGap;
    if (width <= 0 || height <= 0
        || width > std::numeric_limits<int>::max()
        || height > std::numeric_limits<int>::max()
        || width * height > kMaximumContactSheetPixels) {
        return {};
    }
    return {static_cast<int>(width), static_cast<int>(height)};
}

QString ExportPlanner::sanitizedBaseName(QString name)
{
    name = name.trimmed();
    if (name.isEmpty()) {
        name = QStringLiteral("frame");
    }
    for (QChar& character : name) {
        if (QStringLiteral("<>:/\\|?*").contains(character)
            || character == QChar(0x22)
            || character.unicode() < 0x20U) {
            character = QChar(u'_');
        }
    }
    while (name.endsWith(QChar(u'.')) || name.endsWith(QChar(u' '))) {
        name.chop(1);
    }
    return name.isEmpty() ? QStringLiteral("frame") : name.left(96);
}

QString ExportPlanner::frameFileName(
    const QString& baseName,
    const quint64 sequence,
    const media::DecodedFrame& frame,
    const ImageFormat format)
{
    const QString identity = frame.id.presentationIndex >= 0
        ? QStringLiteral("f%1").arg(frame.id.presentationIndex, 8, 10, QChar(u'0'))
        : QStringLiteral("t%1").arg(
              std::max<qint64>(0, static_cast<qint64>(frame.presentationTime.count())),
              19,
              10,
              QChar(u'0'));
    return QStringLiteral("%1_%2_%3.%4")
        .arg(
            sanitizedBaseName(baseName),
            QString::number(sequence).rightJustified(6, QChar(u'0')),
            identity,
            extension(format));
}

QByteArray ExportPlanner::formatName(const ImageFormat format)
{
    switch (format) {
    case ImageFormat::Png:
        return QByteArrayLiteral("png");
    case ImageFormat::Jpeg:
        return QByteArrayLiteral("jpeg");
    case ImageFormat::WebP:
        return QByteArrayLiteral("webp");
    case ImageFormat::Bmp:
        return QByteArrayLiteral("bmp");
    case ImageFormat::Tiff:
        return QByteArrayLiteral("tiff");
    }
    return QByteArrayLiteral("png");
}

QString ExportPlanner::extension(const ImageFormat format)
{
    switch (format) {
    case ImageFormat::Png:
        return QStringLiteral("png");
    case ImageFormat::Jpeg:
        return QStringLiteral("jpg");
    case ImageFormat::WebP:
        return QStringLiteral("webp");
    case ImageFormat::Bmp:
        return QStringLiteral("bmp");
    case ImageFormat::Tiff:
        return QStringLiteral("tiff");
    }
    return QStringLiteral("png");
}

QString ExportPlanner::fileDialogFilter(
    const bool includeTiff,
    const bool includeBmp)
{
    QStringList filters{
        QStringLiteral("PNG image (*.png)"),
        QStringLiteral("JPEG image (*.jpg *.jpeg)"),
        QStringLiteral("WebP image (*.webp)"),
    };
    if (includeBmp) {
        filters.push_back(QStringLiteral("Bitmap image (*.bmp)"));
    }
    if (includeTiff) {
        filters.push_back(QStringLiteral("TIFF image (*.tif *.tiff)"));
    }
    return filters.join(QStringLiteral(";;"));
}

std::optional<ImageFormat> ExportPlanner::formatFromPath(
    const std::filesystem::path& path) noexcept
{
    const QString suffix = QString::fromStdString(path.extension().string()).toLower();
    if (suffix == QStringLiteral(".png")) {
        return ImageFormat::Png;
    }
    if (suffix == QStringLiteral(".jpg") || suffix == QStringLiteral(".jpeg")) {
        return ImageFormat::Jpeg;
    }
    if (suffix == QStringLiteral(".webp")) {
        return ImageFormat::WebP;
    }
    if (suffix == QStringLiteral(".bmp")) {
        return ImageFormat::Bmp;
    }
    if (suffix == QStringLiteral(".tif") || suffix == QStringLiteral(".tiff")) {
        return ImageFormat::Tiff;
    }
    return std::nullopt;
}

} // namespace vidscope::exporting
