#include "TestHarness.h"

#include "export/ExportManager.h"
#include "media/MediaSource.h"
#include "playback/PlaybackController.h"
#include "widgets/MainWindow.h"

#include <QtCore/QByteArray>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QSettings>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtGui/QImage>
#include <QtWidgets/QApplication>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>

using namespace std::chrono_literals;

namespace {

using vidscope::exporting::ContactSheetSource;
using vidscope::exporting::ExportKind;
using vidscope::exporting::ExportManager;
using vidscope::exporting::ExportRequest;
using vidscope::exporting::ExportState;
using vidscope::exporting::ExportSummary;
using vidscope::exporting::ImageFormat;
using vidscope::media::MediaInfoPtr;
using vidscope::playback::PlaybackController;
using vidscope::widgets::MainWindow;

std::filesystem::path fixtureDirectory;

template <typename Predicate>
[[nodiscard]] bool waitUntil(
    Predicate&& predicate,
    const int timeoutMilliseconds = 30'000)
{
    if (predicate()) {
        return true;
    }
    QEventLoop loop;
    QTimer poll;
    QTimer timeout;
    poll.setInterval(2);
    timeout.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (predicate()) {
            loop.quit();
        }
    });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start();
    timeout.start(timeoutMilliseconds);
    loop.exec();
    return predicate();
}

[[nodiscard]] std::filesystem::path temporaryPath(
    const QTemporaryDir& directory,
    const QString& name)
{
#if defined(_WIN32)
    return std::filesystem::path(
        QDir(directory.path()).filePath(name).toStdWString());
#else
    return std::filesystem::path(
        QDir(directory.path()).filePath(name).toStdString());
#endif
}

[[nodiscard]] MediaInfoPtr mediaInfo(const std::filesystem::path& path)
{
    const auto source = vidscope::media::MediaSource::open(path);
    return std::make_shared<vidscope::media::MediaInfo>(source->info());
}

[[nodiscard]] ExportSummary execute(
    const MediaInfoPtr& info,
    ExportRequest request)
{
    ExportManager manager;
    std::optional<ExportSummary> summary;
    QObject::connect(
        &manager,
        &ExportManager::exportFinished,
        [&](const ExportSummary& result) { summary = result; });
    manager.setMedia(info);
    VIDSCOPE_REQUIRE(manager.startExport(std::move(request)));
    VIDSCOPE_REQUIRE(waitUntil([&] { return summary.has_value(); }));
    VIDSCOPE_REQUIRE(!manager.isBusy());
    return *summary;
}

[[nodiscard]] QAction* action(MainWindow& window, const char* name)
{
    return window.findChild<QAction*>(QString::fromLatin1(name));
}

} // namespace

VIDSCOPE_TEST(Phase10_single_frame_export_is_atomic_and_full_resolution)
{
    const auto info = mediaInfo(fixtureDirectory / "cfr_no_b.mp4");
    QTemporaryDir output;
    VIDSCOPE_REQUIRE(output.isValid());
    const auto file = temporaryPath(output, QStringLiteral("current.png"));

    ExportRequest request;
    request.kind = ExportKind::SingleFrame;
    request.outputPath = file;
    request.format = ImageFormat::Png;
    request.anchor = 500ms;
    const auto summary = execute(info, request);
    VIDSCOPE_REQUIRE(summary.state == ExportState::Completed);
    VIDSCOPE_REQUIRE(summary.filesWritten == 1);
    VIDSCOPE_REQUIRE(summary.framesDecoded == 1);
    const QImage image(
        QString::fromStdWString(file.wstring()));
    VIDSCOPE_REQUIRE(!image.isNull());
    VIDSCOPE_REQUIRE(image.size() == QSize(info->width, info->height));

    const auto collision = execute(info, request);
    VIDSCOPE_REQUIRE(collision.state == ExportState::Failed);
    VIDSCOPE_REQUIRE(collision.filesWritten == 0);
    VIDSCOPE_REQUIRE(collision.detail.contains(QStringLiteral("already exists")));
    VIDSCOPE_REQUIRE(!QFileInfo::exists(
        QString::fromStdWString(file.wstring()) + QStringLiteral(".tmp")));

    ExportRequest previous = request;
    previous.relativeFrame = vidscope::exporting::RelativeFrame::Previous;
    previous.outputPath = temporaryPath(output, QStringLiteral("previous.png"));
    const auto previousSummary = execute(info, previous);
    VIDSCOPE_REQUIRE(previousSummary.state == ExportState::Completed);

    ExportRequest next = request;
    next.relativeFrame = vidscope::exporting::RelativeFrame::Next;
    next.outputPath = temporaryPath(output, QStringLiteral("next.png"));
    const auto nextSummary = execute(info, next);
    VIDSCOPE_REQUIRE(nextSummary.state == ExportState::Completed);
    const QImage previousImage(QString::fromStdWString(previous.outputPath.wstring()));
    const QImage nextImage(QString::fromStdWString(next.outputPath.wstring()));
    VIDSCOPE_REQUIRE(previousImage.size() == QSize(info->width, info->height));
    VIDSCOPE_REQUIRE(nextImage.size() == QSize(info->width, info->height));
    VIDSCOPE_REQUIRE(previousImage != nextImage);
}

VIDSCOPE_TEST(Phase10_range_keyframe_and_target_exports_stream_full_frames)
{
    const auto info = mediaInfo(fixtureDirectory / "cfr_no_b.mp4");
    QTemporaryDir output;
    VIDSCOPE_REQUIRE(output.isValid());

    ExportRequest range;
    range.kind = ExportKind::FrameRange;
    range.outputPath = temporaryPath(output, QStringLiteral("range"));
    range.baseName = QStringLiteral("range");
    range.format = ImageFormat::Bmp;
    range.rangeEnd = 400ms;
    range.everyNFrames = 2;
    const auto rangeSummary = execute(info, range);
    VIDSCOPE_REQUIRE(rangeSummary.state == ExportState::Completed);
    VIDSCOPE_REQUIRE(rangeSummary.filesWritten >= 2);
    VIDSCOPE_REQUIRE(rangeSummary.filesWritten <= 4);
    for (const QString& file : rangeSummary.outputFiles) {
        const QImage image(file);
        VIDSCOPE_REQUIRE(image.size() == QSize(info->width, info->height));
    }

    ExportRequest keyframes;
    keyframes.kind = ExportKind::Keyframes;
    keyframes.outputPath = temporaryPath(output, QStringLiteral("keyframes"));
    keyframes.baseName = QStringLiteral("key");
    keyframes.format = ImageFormat::Png;
    keyframes.rangeEnd = info->duration;
    const auto keyframeSummary = execute(info, keyframes);
    VIDSCOPE_REQUIRE(keyframeSummary.state == ExportState::Completed);
    VIDSCOPE_REQUIRE(keyframeSummary.filesWritten >= 2);

    ExportRequest scenes;
    scenes.kind = ExportKind::SceneFrames;
    scenes.outputPath = temporaryPath(output, QStringLiteral("scenes"));
    scenes.baseName = QStringLiteral("scene");
    scenes.format = ImageFormat::WebP;
    scenes.targetTimes = {0ms, 500ms, 1'000ms};
    const auto sceneSummary = execute(info, scenes);
    VIDSCOPE_REQUIRE(sceneSummary.state == ExportState::Completed);
    VIDSCOPE_REQUIRE(sceneSummary.filesWritten == 3);
}

VIDSCOPE_TEST(Phase10_contact_sheet_uses_requested_grid_source_and_labels)
{
    const auto info = mediaInfo(fixtureDirectory / "cfr_bframes.mp4");
    QTemporaryDir output;
    VIDSCOPE_REQUIRE(output.isValid());
    const auto file = temporaryPath(output, QStringLiteral("sheet.jpg"));

    ExportRequest request;
    request.kind = ExportKind::ContactSheet;
    request.outputPath = file;
    request.format = ImageFormat::Jpeg;
    request.rangeEnd = info->duration;
    request.contactSheet.source = ContactSheetSource::EntireVideo;
    request.contactSheet.rows = 2;
    request.contactSheet.columns = 2;
    request.contactSheet.frameCount = 4;
    request.contactSheet.cellSize = QSize(100, 60);
    request.contactSheet.includeTimestamp = false;
    request.contactSheet.includeFrameIndex = false;
    const auto summary = execute(info, request);
    VIDSCOPE_REQUIRE(summary.state == ExportState::Completed);
    VIDSCOPE_REQUIRE(summary.filesWritten == 1);
    VIDSCOPE_REQUIRE(summary.framesDecoded == 4);
    const QImage sheet(QString::fromStdWString(file.wstring()));
    VIDSCOPE_REQUIRE(!sheet.isNull());
    VIDSCOPE_REQUIRE(sheet.size() == QSize(240, 160));
}

VIDSCOPE_TEST(Phase10_manager_rejects_parallel_work_and_cancels_pending_export)
{
    const auto info = mediaInfo(fixtureDirectory / "long_gop.mp4");
    QTemporaryDir output;
    VIDSCOPE_REQUIRE(output.isValid());
    ExportRequest request;
    request.kind = ExportKind::FrameRange;
    request.outputPath = temporaryPath(output, QStringLiteral("cancelled"));
    request.baseName = QStringLiteral("cancel");
    request.rangeEnd = info->duration;

    ExportManager manager;
    std::optional<ExportSummary> summary;
    QObject::connect(
        &manager,
        &ExportManager::exportFinished,
        [&](const ExportSummary& result) { summary = result; });
    manager.setMedia(info);
    VIDSCOPE_REQUIRE(manager.startExport(request));
    VIDSCOPE_REQUIRE(!manager.startExport(request));
    manager.cancel();
    VIDSCOPE_REQUIRE(waitUntil([&] { return summary.has_value(); }));
    VIDSCOPE_REQUIRE(summary->state == ExportState::Cancelled);
    VIDSCOPE_REQUIRE(!manager.isBusy());
}

VIDSCOPE_TEST(Phase10_main_window_exposes_context_aware_export_actions)
{
    QSettings settings;
    settings.clear();
    MainWindow window;
    window.show();
    auto* controller =
        window.findChild<PlaybackController*>(QStringLiteral("playbackController"));
    auto* current = action(window, "actionExportCurrentFrame");
    auto* selected = action(window, "actionExportSelectedFrames");
    auto* everyN = action(window, "actionExportEveryNFrames");
    auto* keyframes = action(window, "actionExportKeyframes");
    auto* scenes = action(window, "actionExportSceneFrames");
    auto* motion = action(window, "actionExportHighMotionFrames");
    auto* sheet = action(window, "actionCreateContactSheet");
    auto* cancel = action(window, "actionCancelExport");
    VIDSCOPE_REQUIRE(controller != nullptr);
    VIDSCOPE_REQUIRE(current != nullptr);
    VIDSCOPE_REQUIRE(selected != nullptr);
    VIDSCOPE_REQUIRE(everyN != nullptr);
    VIDSCOPE_REQUIRE(keyframes != nullptr);
    VIDSCOPE_REQUIRE(scenes != nullptr);
    VIDSCOPE_REQUIRE(motion != nullptr);
    VIDSCOPE_REQUIRE(sheet != nullptr);
    VIDSCOPE_REQUIRE(cancel != nullptr);
    VIDSCOPE_REQUIRE(!current->isEnabled());
    VIDSCOPE_REQUIRE(!cancel->isEnabled());

    const auto mediaPath = fixtureDirectory / "cfr_no_b.mp4";
    controller->openFile(QString::fromStdWString(mediaPath.wstring()));
    VIDSCOPE_REQUIRE(waitUntil([&] { return current->isEnabled(); }));
    VIDSCOPE_REQUIRE(everyN->isEnabled());
    VIDSCOPE_REQUIRE(keyframes->isEnabled());
    VIDSCOPE_REQUIRE(sheet->isEnabled());
    VIDSCOPE_REQUIRE(!selected->isEnabled());
    VIDSCOPE_REQUIRE(!cancel->isEnabled());
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        return 2;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    }
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("VidScopeTests"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("tests.vidscope.invalid"));
    QCoreApplication::setApplicationName(QStringLiteral("Phase10ExportTests"));
    fixtureDirectory = std::filesystem::path(argv[1]);
    return vidscope::test::runAll();
}
