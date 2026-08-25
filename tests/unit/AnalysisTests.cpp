#include "TestHarness.h"

#include "analysis/AnalysisCache.h"
#include "analysis/AnalysisTypes.h"
#include "analysis/VideoAnalyzer.h"

#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <vector>

using namespace std::chrono_literals;

namespace {

using vidscope::analysis::AnalysisCache;
using vidscope::analysis::AnalysisCacheConfig;
using vidscope::analysis::AnalysisSample;
using vidscope::analysis::AnalysisStore;
using vidscope::analysis::LumaPlane;
using vidscope::analysis::VideoAnalyzer;

[[nodiscard]] LumaPlane plane(const std::uint8_t value)
{
    return {4, 4, std::vector<std::uint8_t>(16, value)};
}

[[nodiscard]] std::filesystem::path pathFromQString(const QString& path)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}

[[nodiscard]] AnalysisSample sample(const std::int64_t index, const std::chrono::nanoseconds time)
{
    AnalysisSample result;
    result.presentationIndex = index;
    result.presentationTime = time;
    result.duration = 40ms;
    result.pts = index * 3;
    result.keyFrame = index % 10 == 0;
    result.motion = static_cast<float>(index) / 100.0F;
    result.similarity = 1.0F - *result.motion;
    return result;
}

} // namespace

VIDSCOPE_TEST(VideoAnalyzer_scores_identical_and_maximally_different_luma)
{
    const auto black = plane(0);
    const auto sameBlack = plane(0);
    const auto white = plane(255);

    VIDSCOPE_REQUIRE(VideoAnalyzer::motionScore(black, sameBlack) == 0.0F);
    VIDSCOPE_REQUIRE(VideoAnalyzer::similarityScore(black, sameBlack) == 1.0F);
    VIDSCOPE_REQUIRE(VideoAnalyzer::motionScore(black, white) == 1.0F);
    VIDSCOPE_REQUIRE(VideoAnalyzer::similarityScore(black, white) < 0.01F);
}

VIDSCOPE_TEST(VideoAnalyzer_normalizes_partial_change)
{
    auto changed = plane(0);
    for (std::size_t index = 0; index < changed.pixels.size() / 2; ++index) {
        changed.pixels[index] = 255;
    }
    const float motion = VideoAnalyzer::motionScore(plane(0), changed);
    const float similarity = VideoAnalyzer::similarityScore(plane(0), changed);
    VIDSCOPE_REQUIRE(std::abs(motion - 0.5F) < 0.001F);
    VIDSCOPE_REQUIRE(similarity > 0.4F);
    VIDSCOPE_REQUIRE(similarity < 0.6F);
}

VIDSCOPE_TEST(AnalysisStore_orders_upserts_and_bounds_raw_samples)
{
    AnalysisStore store(3);
    VIDSCOPE_REQUIRE(store.upsert(sample(2, 80ms)));
    VIDSCOPE_REQUIRE(store.upsert(sample(0, 0ms)));
    VIDSCOPE_REQUIRE(store.upsert(sample(1, 40ms)));
    VIDSCOPE_REQUIRE(!store.upsert(sample(3, 120ms)));

    auto replacement = sample(1, 40ms);
    replacement.motion = 0.75F;
    VIDSCOPE_REQUIRE(store.upsert(replacement));
    VIDSCOPE_REQUIRE(store.size() == 3);
    const auto restored = store.nearest(40ms, 1);
    VIDSCOPE_REQUIRE(restored.has_value());
    VIDSCOPE_REQUIRE(restored->motion == replacement.motion);

    const auto range = store.range(20ms, 90ms);
    VIDSCOPE_REQUIRE(range.size() == 2);
    VIDSCOPE_REQUIRE(range[0].presentationIndex == 1);
    VIDSCOPE_REQUIRE(range[1].presentationIndex == 2);
}

VIDSCOPE_TEST(AnalysisCache_round_trips_versioned_compact_samples)
{
    QTemporaryDir directory;
    VIDSCOPE_REQUIRE(directory.isValid());
    const QString mediaPath = directory.filePath(QStringLiteral("source.bin"));
    QFile mediaFile(mediaPath);
    VIDSCOPE_REQUIRE(mediaFile.open(QIODevice::WriteOnly));
    VIDSCOPE_REQUIRE(mediaFile.write("analysis-source", 15) == 15);
    mediaFile.close();

    vidscope::media::MediaInfo info;
    info.path = pathFromQString(mediaPath);
    info.videoStreamIndex = 2;

    AnalysisCacheConfig config;
    config.diskDirectory = directory.filePath(QStringLiteral("cache"));
    config.maximumSamples = 16;
    config.maximumDocumentBytes = 1U * 1024U * 1024U;
    AnalysisCache cache(config);

    std::vector<AnalysisSample> samples{sample(0, 0ms), sample(1, 40ms)};
    samples.front().motion.reset();
    samples.front().similarity.reset();
    VIDSCOPE_REQUIRE(cache.save(info, samples, true));

    const auto restored = cache.load(info);
    VIDSCOPE_REQUIRE(restored.complete);
    VIDSCOPE_REQUIRE(restored.samples == samples);

    VIDSCOPE_REQUIRE(mediaFile.open(QIODevice::Append));
    VIDSCOPE_REQUIRE(mediaFile.write("changed", 7) == 7);
    mediaFile.close();
    VIDSCOPE_REQUIRE(cache.load(info).samples.empty());
}

