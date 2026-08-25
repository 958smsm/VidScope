#include "TestHarness.h"

#include "analysis/AnalysisManager.h"
#include "media/MediaSource.h"

#include <QtCore/QByteArray>
#include <QtCore/QEventLoop>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTimer>
#include <QtCore/QtGlobal>
#include <QtCore/QCoreApplication>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

using namespace std::chrono_literals;

namespace {

using vidscope::analysis::AnalysisManager;
using vidscope::analysis::AnalysisManagerConfig;
using vidscope::analysis::AnalysisState;
using vidscope::media::MediaInfo;
using vidscope::media::MediaInfoPtr;
using vidscope::media::MediaSource;

std::filesystem::path fixtureDirectory;

[[nodiscard]] std::filesystem::path fixturePath(const char* name)
{
    const auto path = fixtureDirectory / name;
    VIDSCOPE_REQUIRE_MESSAGE(std::filesystem::exists(path), path.string());
    return path;
}

[[nodiscard]] MediaInfoPtr loadMediaInfo(const std::filesystem::path& path)
{
    auto source = MediaSource::open(path);
    VIDSCOPE_REQUIRE(source != nullptr);
    return std::make_shared<MediaInfo>(source->info());
}

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate&& predicate, const int timeoutMilliseconds = 30'000)
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

[[nodiscard]] AnalysisManagerConfig configuration(const QString& cacheDirectory)
{
    AnalysisManagerConfig config;
    config.cache.diskDirectory = cacheDirectory;
    config.cache.diskBudgetBytes = 16U * 1024U * 1024U;
    config.cache.maximumDocumentBytes = 8U * 1024U * 1024U;
    config.maximumInMemorySamples = 1'024;
    config.cache.maximumSamples = 1'024;
    config.deliveryBatchFrames = 4;
    config.session.frameCacheBytes = 4U * 1024U * 1024U;
    config.session.forwardQueueBytes = 2U * 1024U * 1024U;
    config.session.forwardQueueFrames = 2;
    config.session.initialPrefetchFrames = 1;
    return config;
}

} // namespace

VIDSCOPE_TEST(Phase6_progressively_analyzes_real_decoded_frames_and_persists_cache)
{
    QTemporaryDir cacheDirectory;
    VIDSCOPE_REQUIRE(cacheDirectory.isValid());
    const auto info = loadMediaInfo(fixturePath("cfr_no_b.mp4"));
    qsizetype completedCount = 0;

    {
        AnalysisManager manager(configuration(cacheDirectory.path()));
        manager.setMedia(info);
        VIDSCOPE_REQUIRE(waitUntil([&] { return manager.sampleCount() > 0; }));
        VIDSCOPE_REQUIRE(waitUntil([&] { return manager.state() == AnalysisState::Complete; }));
        VIDSCOPE_REQUIRE(manager.progress() == 1.0);
        completedCount = manager.sampleCount();
        VIDSCOPE_REQUIRE(completedCount >= 20);

        const auto samples = manager.samplesInRange(0, static_cast<qint64>(info->duration.count()));
        VIDSCOPE_REQUIRE(static_cast<qsizetype>(samples.size()) == completedCount);
        VIDSCOPE_REQUIRE(!samples.front().motion.has_value());
        VIDSCOPE_REQUIRE(!samples.front().similarity.has_value());
        for (std::size_t index = 1; index < samples.size(); ++index) {
            VIDSCOPE_REQUIRE(samples[index].motion.has_value());
            VIDSCOPE_REQUIRE(samples[index].similarity.has_value());
            VIDSCOPE_REQUIRE(*samples[index].motion >= 0.0F);
            VIDSCOPE_REQUIRE(*samples[index].motion <= 1.0F);
            VIDSCOPE_REQUIRE(*samples[index].similarity >= 0.0F);
            VIDSCOPE_REQUIRE(*samples[index].similarity <= 1.0F);
            VIDSCOPE_REQUIRE(samples[index - 1].presentationTime <= samples[index].presentationTime);
        }
    }

    AnalysisManager restored(configuration(cacheDirectory.path()));
    restored.setMedia(info);
    VIDSCOPE_REQUIRE(waitUntil([&] { return restored.state() == AnalysisState::Complete; }));
    VIDSCOPE_REQUIRE(restored.sampleCount() == completedCount);
    VIDSCOPE_REQUIRE(restored.progress() == 1.0);
}

VIDSCOPE_TEST(Phase6_playback_activity_pauses_background_decode_and_resumes_cleanly)
{
    QTemporaryDir cacheDirectory;
    VIDSCOPE_REQUIRE(cacheDirectory.isValid());
    const auto info = loadMediaInfo(fixturePath("long_gop.mp4"));
    AnalysisManager manager(configuration(cacheDirectory.path()));

    manager.setPlaybackActive(true);
    manager.setMedia(info);
    QEventLoop delay;
    QTimer::singleShot(250, &delay, &QEventLoop::quit);
    delay.exec();
    VIDSCOPE_REQUIRE(manager.sampleCount() == 0);

    manager.setPlaybackActive(false);
    manager.requestPlayhead(3'000'000'000LL);
    VIDSCOPE_REQUIRE(waitUntil([&] { return manager.sampleCount() > 0; }));
    VIDSCOPE_REQUIRE(waitUntil([&] { return manager.state() == AnalysisState::Complete; }));
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        return 2;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    }
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("VidScopeTests"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("tests.vidscope.invalid"));
    QCoreApplication::setApplicationName(QStringLiteral("Phase6AnalysisTests"));
    fixtureDirectory = std::filesystem::path(argv[1]);
    return vidscope::test::runAll();
}
