#include "analysis/AnalysisManager.h"
#include "analysis/AnalysisPyramid.h"
#include "analysis/VideoAnalyzer.h"
#include "media/FrameConverter.h"
#include "media/MediaSource.h"
#include "playback/PlaybackSession.h"
#include "thumbnails/ThumbnailCache.h"
#include "timeline/TimelineHeatmapRenderer.h"

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QTemporaryDir>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtGui/QPainter>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;
const auto kModuleLoadTime = Clock::now();

struct Results final {
    double qtStartupMilliseconds = 0.0;
    double mediaOpenMilliseconds = 0.0;
    double softwareDecodeFps = 0.0;
    double seekMedianMilliseconds = 0.0;
    double seekP95Milliseconds = 0.0;
    double thumbnailMedianMilliseconds = 0.0;
    double legacyAnalysisKernelFps = 0.0;
    double analysisKernelFps = 0.0;
    double realMediaAnalysisFps = 0.0;
    double freshLumaExtractionsPerSecond = 0.0;
    double reusedLumaExtractionsPerSecond = 0.0;
    double pyramidRebuildMilliseconds = 0.0;
    double pyramidViewsPerSecond = 0.0;
    double concurrentViewsPerSecond = 0.0;
    double thumbnailCacheLookupsPerSecond = 0.0;
    double contendedCacheLookupsPerSecond = 0.0;
    double thumbnailCacheHitPercent = 0.0;
    double timelineRasterizeMilliseconds = 0.0;
    double timelineCachedPaintMilliseconds = 0.0;
    std::size_t timelineBucketCount = 0;
    bool hardwareDecodeActive = false;
    std::uint64_t checksum = 0;
};

[[nodiscard]] double milliseconds(const Clock::duration duration) noexcept
{
    return std::chrono::duration<double, std::milli>(duration).count();
}

[[nodiscard]] double seconds(const Clock::duration duration) noexcept
{
    return std::chrono::duration<double>(duration).count();
}

[[nodiscard]] double percentileMilliseconds(
    std::vector<Clock::duration> samples,
    const double percentile)
{
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    const auto position = static_cast<std::size_t>(std::ceil(
        std::clamp(percentile, 0.0, 1.0) * static_cast<double>(samples.size()))) - 1U;
    return milliseconds(samples[std::min(position, samples.size() - 1U)]);
}

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate&& predicate, const int timeoutMilliseconds)
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

[[nodiscard]] vidscope::analysis::LumaPlane makePlane(const std::uint32_t seed)
{
    vidscope::analysis::LumaPlane plane;
    plane.width = 160;
    plane.height = 90;
    plane.pixels.resize(static_cast<std::size_t>(plane.width * plane.height));
    std::uint32_t state = seed;
    for (auto& pixel : plane.pixels) {
        state = state * 1'664'525U + 1'013'904'223U;
        pixel = static_cast<std::uint8_t>(state >> 24U);
    }
    return plane;
}

void profileAnalysisKernel(Results& results)
{
    constexpr std::size_t iterations = 2'000;
    const auto run = [&](const bool reuseHashes) {
        auto previous = makePlane(0x1234U);
        auto current = makePlane(0x5678U);
        auto previousPerceptualHash =
            vidscope::analysis::VideoAnalyzer::perceptualHash(previous);
        std::uint64_t checksum = 0;
        const auto started = Clock::now();
        for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
            current.pixels[iteration % current.pixels.size()] ^=
                static_cast<std::uint8_t>(iteration);
            const auto contentHash = vidscope::analysis::VideoAnalyzer::contentHash(current);
            const auto perceptualHash =
                vidscope::analysis::VideoAnalyzer::perceptualHash(current);
            const auto metrics = reuseHashes
                ? vidscope::analysis::VideoAnalyzer::compare(
                    previous,
                    current,
                    previousPerceptualHash,
                    perceptualHash)
                : vidscope::analysis::VideoAnalyzer::compare(previous, current);
            checksum ^= contentHash ^ perceptualHash;
            checksum += static_cast<std::uint64_t>(metrics.duplicate * 1'000'000.0F);
            std::swap(previous, current);
            previousPerceptualHash = perceptualHash;
        }
        results.checksum ^= checksum;
        return static_cast<double>(iterations) / seconds(Clock::now() - started);
    };
    results.legacyAnalysisKernelFps = run(false);
    results.analysisKernelFps = run(true);
}

[[nodiscard]] vidscope::analysis::AnalysisLodView profilePyramid(Results& results)
{
    constexpr std::size_t sampleCount = 180'000;
    const auto duration = std::chrono::hours(2);
    const auto durationNanoseconds =
        std::chrono::duration_cast<vidscope::media::MediaTime>(duration);
    std::vector<vidscope::analysis::AnalysisSample> samples;
    samples.reserve(sampleCount);
    for (std::size_t index = 0; index < sampleCount; ++index) {
        vidscope::analysis::AnalysisSample sample;
        sample.presentationTime = vidscope::media::MediaTime(
            durationNanoseconds.count() * static_cast<std::int64_t>(index)
            / static_cast<std::int64_t>(sampleCount));
        sample.presentationIndex = static_cast<std::int64_t>(index);
        sample.motion = static_cast<float>(index % 101U) / 100.0F;
        sample.similarity = 1.0F - *sample.motion * 0.5F;
        sample.sceneScore = index % 503U == 0U ? 0.95F : 0.03F;
        samples.push_back(sample);
    }

    vidscope::analysis::AnalysisPyramid pyramid;
    pyramid.reset(durationNanoseconds, sampleCount);
    const auto rebuildStarted = Clock::now();
    pyramid.rebuild(samples);
    results.pyramidRebuildMilliseconds = milliseconds(Clock::now() - rebuildStarted);

    constexpr std::size_t viewIterations = 1'200;
    const auto viewStarted = Clock::now();
    for (std::size_t iteration = 0; iteration < viewIterations; ++iteration) {
        const auto offset = std::chrono::seconds(static_cast<std::int64_t>(iteration % 3'000U));
        const auto view = pyramid.view(
            std::chrono::duration_cast<vidscope::media::MediaTime>(offset),
            std::chrono::duration_cast<vidscope::media::MediaTime>(offset + 15min),
            1'920);
        results.checksum += view.buckets.size();
    }
    results.pyramidViewsPerSecond = static_cast<double>(viewIterations)
        / seconds(Clock::now() - viewStarted);

    constexpr std::size_t threadCount = 4;
    constexpr std::size_t viewsPerThread = 600;
    std::atomic<std::uint64_t> concurrentChecksum{0};
    const auto contentionStarted = Clock::now();
    std::vector<std::jthread> readers;
    readers.reserve(threadCount);
    for (std::size_t thread = 0; thread < threadCount; ++thread) {
        readers.emplace_back([&, thread] {
            std::uint64_t local = 0;
            for (std::size_t iteration = 0; iteration < viewsPerThread; ++iteration) {
                const auto offset = std::chrono::seconds(
                    static_cast<std::int64_t>((iteration * 17U + thread * 31U) % 3'000U));
                local += pyramid.view(
                    std::chrono::duration_cast<vidscope::media::MediaTime>(offset),
                    std::chrono::duration_cast<vidscope::media::MediaTime>(offset + 15min),
                    1'920).buckets.size();
            }
            concurrentChecksum.fetch_add(local, std::memory_order_relaxed);
        });
    }
    readers.clear();
    results.concurrentViewsPerSecond = static_cast<double>(threadCount * viewsPerThread)
        / seconds(Clock::now() - contentionStarted);
    results.checksum += concurrentChecksum.load(std::memory_order_relaxed);
    return pyramid.view(
        vidscope::media::MediaTime::zero(),
        std::chrono::duration_cast<vidscope::media::MediaTime>(duration),
        1'920);
}

void profileTimelinePaint(
    Results& results,
    const vidscope::analysis::AnalysisLodView& view)
{
    results.timelineBucketCount = view.buckets.size();
    QImage target(QSize(1'920, 112), QImage::Format_ARGB32_Premultiplied);
    vidscope::timeline::TimelineHeatmapRenderer renderer;
    constexpr std::size_t iterations = 180;
    const auto started = Clock::now();
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        target.fill(Qt::transparent);
        QPainter painter(&target);
        renderer.paint(
            painter,
            QRectF(0.0, 0.0, 1'920.0, 112.0),
            view,
            vidscope::timeline::HeatmapMode::Combined);
    }
    results.timelineRasterizeMilliseconds = milliseconds(Clock::now() - started)
        / static_cast<double>(iterations);

    QImage cachedLayer(target.size(), QImage::Format_ARGB32_Premultiplied);
    cachedLayer.fill(Qt::transparent);
    {
        QPainter cachePainter(&cachedLayer);
        renderer.paint(
            cachePainter,
            QRectF(0.0, 0.0, 1'920.0, 112.0),
            view,
            vidscope::timeline::HeatmapMode::Combined);
    }
    const auto cachedStarted = Clock::now();
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        target.fill(Qt::transparent);
        QPainter painter(&target);
        painter.drawImage(QPoint(0, 0), cachedLayer);
    }
    results.timelineCachedPaintMilliseconds = milliseconds(Clock::now() - cachedStarted)
        / static_cast<double>(iterations);
    results.checksum += static_cast<std::uint64_t>(target.pixel(960, 56));
}

void profileThumbnailCache(Results& results)
{
    vidscope::thumbnails::ThumbnailCacheConfig config;
    config.memoryBudgetBytes = 16U * 1024U * 1024U;
    config.diskBudgetBytes = 0;
    vidscope::thumbnails::ThumbnailCache cache(config);
    std::vector<vidscope::thumbnails::ThumbnailCacheKey> keys;
    keys.reserve(48);
    for (std::size_t index = 0; index < 48U; ++index) {
        vidscope::thumbnails::ThumbnailCacheKey key;
        key.mediaIdentity = QStringLiteral("phase11-profile");
        key.timestampNanoseconds = static_cast<qint64>(index) * 40'000'000LL;
        key.targetSize = QSize(320, 180);
        vidscope::thumbnails::ThumbnailFrame frame;
        frame.image = QImage(key.targetSize, QImage::Format_ARGB32);
        frame.image.fill(QColor::fromRgb(static_cast<QRgb>(index + 1U)));
        cache.insertMemory(key, frame);
        keys.push_back(std::move(key));
    }

    constexpr std::size_t lookupCount = 120'000;
    std::size_t hitCount = 0;
    const auto singleStarted = Clock::now();
    for (std::size_t index = 0; index < lookupCount; ++index) {
        const auto found = cache.lookupMemory(keys[index % keys.size()]);
        hitCount += found.has_value() ? 1U : 0U;
    }
    results.thumbnailCacheLookupsPerSecond = static_cast<double>(lookupCount)
        / seconds(Clock::now() - singleStarted);
    results.thumbnailCacheHitPercent = 100.0 * static_cast<double>(hitCount)
        / static_cast<double>(lookupCount);

    constexpr std::size_t threadCount = 4;
    constexpr std::size_t lookupsPerThread = 30'000;
    std::atomic<std::uint64_t> concurrentHits{0};
    const auto contentionStarted = Clock::now();
    std::vector<std::jthread> readers;
    readers.reserve(threadCount);
    for (std::size_t thread = 0; thread < threadCount; ++thread) {
        readers.emplace_back([&, thread] {
            std::uint64_t localHits = 0;
            for (std::size_t index = 0; index < lookupsPerThread; ++index) {
                const auto keyIndex = (index * 13U + thread * 7U) % keys.size();
                localHits += cache.lookupMemory(keys[keyIndex]).has_value() ? 1U : 0U;
            }
            concurrentHits.fetch_add(localHits, std::memory_order_relaxed);
        });
    }
    readers.clear();
    results.contendedCacheLookupsPerSecond =
        static_cast<double>(threadCount * lookupsPerThread)
        / seconds(Clock::now() - contentionStarted);
    results.checksum += concurrentHits.load(std::memory_order_relaxed);
}

void profileMedia(
    Results& results,
    const std::filesystem::path& fixtureDirectory)
{
    const auto mediaPath = fixtureDirectory / "long_gop.mp4";
    if (!std::filesystem::exists(mediaPath)) {
        throw std::runtime_error("The Phase 11 media fixture is missing");
    }

    const auto openStarted = Clock::now();
    auto source = vidscope::media::MediaSource::open(mediaPath);
    results.mediaOpenMilliseconds = milliseconds(Clock::now() - openStarted);
    if (!source) {
        throw std::runtime_error("Could not open the Phase 11 media fixture");
    }
    const auto info = std::make_shared<vidscope::media::MediaInfo>(source->info());
    source.reset();

    vidscope::playback::PlaybackSessionConfig sessionConfig;
    sessionConfig.decoder.hardwareAcceleration = vidscope::media::HardwareAcceleration::Disabled;
    sessionConfig.frameCacheBytes = 16U * 1024U * 1024U;
    sessionConfig.forwardQueueBytes = 4U * 1024U * 1024U;
    sessionConfig.forwardQueueFrames = 4;
    sessionConfig.initialPrefetchFrames = 2;
    vidscope::playback::PlaybackSession session(sessionConfig);
    auto navigation = session.open(mediaPath);
    if (!navigation) {
        throw std::runtime_error("Could not decode the first Phase 11 media frame");
    }

    vidscope::analysis::LumaExtractor extractor;
    constexpr std::size_t extractionIterations = 2'000;
    auto extracted = extractor.extract(*navigation.frame);
    vidscope::analysis::LumaPlane reusable;
    if (!extractor.extract(*navigation.frame, reusable)
        || reusable.width != extracted.width
        || reusable.height != extracted.height
        || reusable.pixels != extracted.pixels) {
        throw std::runtime_error("Reusable luma extraction changed the decoded result");
    }
    const auto* reusableStorage = reusable.pixels.data();
    if (!extractor.extract(*navigation.frame, reusable)
        || reusable.pixels.data() != reusableStorage) {
        throw std::runtime_error("Reusable luma extraction did not retain its allocation");
    }
    const auto freshExtractionStarted = Clock::now();
    for (std::size_t iteration = 0; iteration < extractionIterations; ++iteration) {
        extracted = extractor.extract(*navigation.frame);
        results.checksum += extracted.pixels[iteration % extracted.pixels.size()];
    }
    results.freshLumaExtractionsPerSecond = static_cast<double>(extractionIterations)
        / seconds(Clock::now() - freshExtractionStarted);

    const auto reusedExtractionStarted = Clock::now();
    for (std::size_t iteration = 0; iteration < extractionIterations; ++iteration) {
        if (!extractor.extract(*navigation.frame, reusable)) {
            throw std::runtime_error("The reusable Phase 11 luma extraction failed");
        }
        results.checksum += reusable.pixels[iteration % reusable.pixels.size()];
    }
    results.reusedLumaExtractionsPerSecond = static_cast<double>(extractionIterations)
        / seconds(Clock::now() - reusedExtractionStarted);

    std::size_t decodedFrames = 0;
    const auto decodeStarted = Clock::now();
    while (navigation && decodedFrames < 120U) {
        ++decodedFrames;
        navigation = session.nextFrame();
    }
    results.softwareDecodeFps = static_cast<double>(decodedFrames)
        / seconds(Clock::now() - decodeStarted);

    std::vector<Clock::duration> seekSamples;
    std::vector<Clock::duration> thumbnailSamples;
    vidscope::media::FrameConverter converter;
    std::uint64_t generation = 1;
    for (int index = 0; index < 16; ++index) {
        const auto target = vidscope::media::MediaTime(
            info->duration.count() * static_cast<std::int64_t>((index * 7) % 16) / 16);
        const auto seekStarted = Clock::now();
        navigation = session.seek({generation++, target, vidscope::playback::SeekBias::Nearest});
        seekSamples.push_back(Clock::now() - seekStarted);
        if (!navigation || !navigation.frame) {
            continue;
        }
        const auto thumbnailStarted = Clock::now();
        const QImage image = converter.toBgraImage(*navigation.frame, QSize(320, 180));
        thumbnailSamples.push_back(Clock::now() - thumbnailStarted);
        results.checksum += static_cast<std::uint64_t>(image.sizeInBytes());
    }
    results.seekMedianMilliseconds = percentileMilliseconds(seekSamples, 0.50);
    results.seekP95Milliseconds = percentileMilliseconds(seekSamples, 0.95);
    results.thumbnailMedianMilliseconds = percentileMilliseconds(thumbnailSamples, 0.50);

    vidscope::playback::PlaybackSession hardwareSession;
    const auto hardwareOpen = hardwareSession.open(mediaPath);
    results.hardwareDecodeActive = hardwareOpen && hardwareSession.usesHardwareAcceleration();

    QTemporaryDir cacheDirectory;
    if (!cacheDirectory.isValid()) {
        throw std::runtime_error("Could not create the Phase 11 analysis cache directory");
    }
    vidscope::analysis::AnalysisManagerConfig analysisConfig;
    analysisConfig.cache.diskDirectory = cacheDirectory.path();
    analysisConfig.cache.diskBudgetBytes = 16U * 1024U * 1024U;
    analysisConfig.cache.maximumDocumentBytes = 8U * 1024U * 1024U;
    analysisConfig.maximumInMemorySamples = 4'096;
    analysisConfig.cache.maximumSamples = 4'096;
    analysisConfig.deliveryBatchFrames = 16;
    analysisConfig.session.decoder.hardwareAcceleration =
        vidscope::media::HardwareAcceleration::Disabled;
    vidscope::analysis::AnalysisManager manager(analysisConfig);
    const auto analysisStarted = Clock::now();
    manager.setMedia(info);
    const bool completed = waitUntil([&] {
        return manager.state() == vidscope::analysis::AnalysisState::Complete
            || manager.state() == vidscope::analysis::AnalysisState::Error;
    }, 60'000);
    if (!completed || manager.state() != vidscope::analysis::AnalysisState::Complete) {
        throw std::runtime_error("The Phase 11 real-media analysis did not complete");
    }
    results.realMediaAnalysisFps = static_cast<double>(manager.sampleCount())
        / seconds(Clock::now() - analysisStarted);
    results.checksum += static_cast<std::uint64_t>(manager.sampleCount());
}

void writeResults(const Results& results)
{
    QTextStream output(stdout);
    output.setRealNumberNotation(QTextStream::FixedNotation);
    output.setRealNumberPrecision(3);
    output << "qt_startup_ms=" << results.qtStartupMilliseconds << '\n'
           << "media_open_ms=" << results.mediaOpenMilliseconds << '\n'
           << "software_decode_fps=" << results.softwareDecodeFps << '\n'
           << "seek_median_ms=" << results.seekMedianMilliseconds << '\n'
           << "seek_p95_ms=" << results.seekP95Milliseconds << '\n'
           << "thumbnail_convert_median_ms=" << results.thumbnailMedianMilliseconds << '\n'
           << "legacy_analysis_kernel_fps=" << results.legacyAnalysisKernelFps << '\n'
           << "analysis_kernel_fps=" << results.analysisKernelFps << '\n'
           << "real_media_analysis_fps=" << results.realMediaAnalysisFps << '\n'
           << "fresh_luma_extractions_per_second="
           << results.freshLumaExtractionsPerSecond << '\n'
           << "reused_luma_extractions_per_second="
           << results.reusedLumaExtractionsPerSecond << '\n'
           << "pyramid_rebuild_ms=" << results.pyramidRebuildMilliseconds << '\n'
           << "pyramid_views_per_second=" << results.pyramidViewsPerSecond << '\n'
           << "concurrent_views_per_second=" << results.concurrentViewsPerSecond << '\n'
           << "thumbnail_cache_lookups_per_second="
           << results.thumbnailCacheLookupsPerSecond << '\n'
           << "contended_cache_lookups_per_second="
           << results.contendedCacheLookupsPerSecond << '\n'
           << "thumbnail_cache_hit_percent=" << results.thumbnailCacheHitPercent << '\n'
           << "timeline_rasterize_ms=" << results.timelineRasterizeMilliseconds << '\n'
           << "timeline_cached_paint_ms=" << results.timelineCachedPaintMilliseconds << '\n'
           << "timeline_bucket_count=" << results.timelineBucketCount << '\n'
           << "hardware_decode_active="
           << (results.hardwareDecodeActive ? "true" : "false") << '\n'
           << "checksum=" << results.checksum << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        return 2;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    }
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("VidScopeTests"));
    QCoreApplication::setApplicationName(QStringLiteral("Phase11Benchmarks"));

    Results results;
    results.qtStartupMilliseconds = milliseconds(Clock::now() - kModuleLoadTime);
    try {
        profileAnalysisKernel(results);
        const auto view = profilePyramid(results);
        profileTimelinePaint(results, view);
        profileThumbnailCache(results);
        profileMedia(results, std::filesystem::path(argv[1]));
        writeResults(results);
        return 0;
    } catch (const std::exception& error) {
        QTextStream(stderr) << "Phase 11 benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
