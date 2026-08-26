#include "TestHarness.h"

#include "media/MediaSource.h"
#include "playback/PlaybackController.h"
#include "timeline/TimelineWidget.h"
#include "widgets/MainWindow.h"
#include "widgets/ProfessionalPanel.h"

#include <QtCore/QByteArray>
#include <QtCore/QEventLoop>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtGui/QAction>
#include <QtWidgets/QApplication>
#include <QtWidgets/QDockWidget>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QTreeWidget>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <vector>

using namespace std::chrono_literals;

namespace {

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

[[nodiscard]] QAction* action(
    vidscope::widgets::MainWindow& window,
    const char* name)
{
    return window.findChild<QAction*>(QString::fromLatin1(name));
}

} // namespace

VIDSCOPE_TEST(Phase12_media_source_preserves_chapter_titles_and_boundaries)
{
    const auto source = vidscope::media::MediaSource::open(
        fixtureDirectory / "chapters.mkv");
    const auto& chapters = source->info().chapters;
    VIDSCOPE_REQUIRE(chapters.size() == 2);
    VIDSCOPE_REQUIRE(chapters[0].title == "Opening");
    VIDSCOPE_REQUIRE(chapters[0].start == 0ms);
    VIDSCOPE_REQUIRE(chapters[0].end == 1s);
    VIDSCOPE_REQUIRE(chapters[1].title == "Detail");
    VIDSCOPE_REQUIRE(chapters[1].start == 1s);
}

VIDSCOPE_TEST(Phase12_professional_panel_surfaces_history_markers_and_diagnostics)
{
    vidscope::widgets::ProfessionalPanel panel;
    const std::vector<vidscope::inspection::FrameHistoryEntry> history{
        {{1, 10, 1}, 100ms, 40ms, true, AV_PICTURE_TYPE_I},
        {{2, 20, 2}, 140ms, 40ms, false, AV_PICTURE_TYPE_P},
    };
    panel.setHistory(history, 1);
    auto* historyTree =
        panel.findChild<QTreeWidget*>(QStringLiteral("frameHistory"));
    VIDSCOPE_REQUIRE(historyTree != nullptr);
    VIDSCOPE_REQUIRE(historyTree->topLevelItemCount() == 2);

    const std::vector<vidscope::timeline::TimelineMarker> markers{
        {7,
         1s,
         vidscope::timeline::TimelineMarkerKind::Bookmark,
         QStringLiteral("Check"),
         QStringLiteral("Issue"),
         QStringLiteral("Review this frame.")},
    };
    panel.setMarkers(markers);
    auto* markerTree =
        panel.findChild<QTreeWidget*>(QStringLiteral("markerNotes"));
    VIDSCOPE_REQUIRE(markerTree != nullptr);
    VIDSCOPE_REQUIRE(markerTree->topLevelItemCount() == 1);
    VIDSCOPE_REQUIRE(markerTree->topLevelItem(0)->text(1) == QStringLiteral("Issue"));
    VIDSCOPE_REQUIRE(
        markerTree->topLevelItem(0)->text(4) == QStringLiteral("Review this frame."));

    vidscope::playback::PlaybackDiagnostics diagnostics;
    diagnostics.decodeFramesPerSecond = 24.0;
    diagnostics.bufferedFrames = 6;
    diagnostics.droppedFrameDeliveries = 2;
    diagnostics.hardwareDecodeActive = true;
    diagnostics.hardwareDevice = QStringLiteral("d3d11va");
    panel.setDiagnostics(diagnostics);
    auto* text = panel.findChild<QPlainTextEdit*>(
        QStringLiteral("playbackDiagnostics"));
    VIDSCOPE_REQUIRE(text != nullptr);
    VIDSCOPE_REQUIRE(text->toPlainText().contains(QStringLiteral("24.0 fps")));
    VIDSCOPE_REQUIRE(text->toPlainText().contains(QStringLiteral("d3d11va")));
}

VIDSCOPE_TEST(Phase12_main_window_installs_chapters_history_and_professional_actions)
{
    QSettings settings;
    settings.clear();
    vidscope::widgets::MainWindow window;
    window.show();
    auto* controller = window.findChild<vidscope::playback::PlaybackController*>(
        QStringLiteral("playbackController"));
    auto* timeline = window.findChild<vidscope::timeline::TimelineWidget*>();
    auto* professional = window.findChild<vidscope::widgets::ProfessionalPanel*>(
        QStringLiteral("professionalPanel"));
    auto* dock = window.findChild<QDockWidget*>(
        QStringLiteral("professionalToolsDock"));
    auto* previousChapter = action(window, "actionPreviousChapter");
    auto* nextChapter = action(window, "actionNextChapter");
    auto* historyBack = action(window, "actionHistoryBack");
    auto* historyForward = action(window, "actionHistoryForward");
    auto* annotated = action(window, "actionAddAnnotatedMarker");
    auto* visual = action(window, "actionVisualSearch");
    VIDSCOPE_REQUIRE(controller != nullptr);
    VIDSCOPE_REQUIRE(timeline != nullptr);
    VIDSCOPE_REQUIRE(professional != nullptr);
    VIDSCOPE_REQUIRE(dock != nullptr);
    VIDSCOPE_REQUIRE(previousChapter != nullptr);
    VIDSCOPE_REQUIRE(nextChapter != nullptr);
    VIDSCOPE_REQUIRE(historyBack != nullptr);
    VIDSCOPE_REQUIRE(historyForward != nullptr);
    VIDSCOPE_REQUIRE(annotated != nullptr);
    VIDSCOPE_REQUIRE(visual != nullptr);

    const auto mediaPath = fixtureDirectory / "chapters.mkv";
    controller->openFile(QString::fromStdWString(mediaPath.wstring()));
    auto* historyTree = professional->findChild<QTreeWidget*>(
        QStringLiteral("frameHistory"));
    auto* diagnostics = professional->findChild<QPlainTextEdit*>(
        QStringLiteral("playbackDiagnostics"));
    VIDSCOPE_REQUIRE(waitUntil([&] {
        return historyTree != nullptr
            && diagnostics != nullptr
            && historyTree->topLevelItemCount() > 0
            && !diagnostics->toPlainText().isEmpty();
    }));
    const auto chapterCount = std::count_if(
        timeline->model().markers().begin(),
        timeline->model().markers().end(),
        [](const vidscope::timeline::TimelineMarker& marker) {
            return marker.kind
                == vidscope::timeline::TimelineMarkerKind::Chapter;
        });
    VIDSCOPE_REQUIRE(chapterCount == 2);
    VIDSCOPE_REQUIRE(previousChapter->isEnabled());
    VIDSCOPE_REQUIRE(nextChapter->isEnabled());
    VIDSCOPE_REQUIRE(annotated->isEnabled());
    VIDSCOPE_REQUIRE(visual->isEnabled());
    VIDSCOPE_REQUIRE(!historyForward->isEnabled());
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
    QCoreApplication::setApplicationName(QStringLiteral("Phase12ProfessionalTests"));
    fixtureDirectory = std::filesystem::path(argv[1]);
    return vidscope::test::runAll();
}
