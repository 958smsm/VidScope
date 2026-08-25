#include "TestHarness.h"

#include "analysis/AnalysisCache.h"
#include "analysis/AnalysisPyramid.h"
#include "analysis/AnalysisTypes.h"
#include "analysis/DetectionEngine.h"
#include "analysis/VideoAnalyzer.h"

#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <vector>

using namespace std::chrono_literals;

namespace {

using vidscope::analysis::AnalysisCache;
using vidscope::analysis::AnalysisCacheConfig;
using vidscope::analysis::AnalysisPyramid;
using vidscope::analysis::AnalysisPyramidConfig;
using vidscope::analysis::AnalysisSample;
using vidscope::analysis::AnalysisStore;
using vidscope::analysis::DetectionConfig;
using vidscope::analysis::DetectionEngine;
using vidscope::analysis::DetectionKind;
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

VIDSCOPE_TEST(VideoAnalyzer_exposes_scene_duplicate_and_stable_fingerprint_metrics)
{
    const auto black = plane(0);
    const auto white = plane(255);
    const auto identical = VideoAnalyzer::compare(black, black);
    const auto cut = VideoAnalyzer::compare(black, white);
    VIDSCOPE_REQUIRE(identical.duplicate == 1.0F);
    VIDSCOPE_REQUIRE(identical.sceneChange == 0.0F);
    VIDSCOPE_REQUIRE(cut.sceneChange > 0.99F);
    VIDSCOPE_REQUIRE(cut.duplicate < 0.01F);
    VIDSCOPE_REQUIRE(VideoAnalyzer::contentHash(black) == VideoAnalyzer::contentHash(black));
    VIDSCOPE_REQUIRE(VideoAnalyzer::contentHash(black) != VideoAnalyzer::contentHash(white));
    VIDSCOPE_REQUIRE(VideoAnalyzer::perceptualHash(black)
        != VideoAnalyzer::perceptualHash(white));
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

VIDSCOPE_TEST(AnalysisPyramid_builds_four_to_one_levels_and_preserves_raw_statistics)
{
    AnalysisPyramidConfig config;
    config.maximumBaseBuckets = 16;
    AnalysisPyramid pyramid(config);
    pyramid.reset(16s, 16);

    std::vector<AnalysisSample> samples;
    for (std::int64_t index = 0; index < 16; ++index) {
        samples.push_back(sample(index, std::chrono::seconds(index)));
    }
    pyramid.rebuild(samples);

    const auto overview = pyramid.view(0s, 16s, 4);
    VIDSCOPE_REQUIRE(overview.level == 1);
    VIDSCOPE_REQUIRE(overview.sourceBucketsPerBucket == 4);
    VIDSCOPE_REQUIRE(overview.buckets.size() == 4);
    VIDSCOPE_REQUIRE(overview.buckets.front().sampleCount == 4);
    VIDSCOPE_REQUIRE(std::abs(overview.buckets.front().averageMotion - 0.015F) < 0.0001F);
    VIDSCOPE_REQUIRE(overview.buckets.front().minMotion == 0.0F);
    VIDSCOPE_REQUIRE(std::abs(overview.buckets.front().maxMotion - 0.03F) < 0.0001F);
    VIDSCOPE_REQUIRE(std::abs(overview.buckets.front().averageSimilarity - 0.985F) < 0.0001F);

    auto replacement = sample(5, 5s);
    replacement.motion = 1.0F;
    replacement.similarity = 0.0F;
    const std::vector<AnalysisSample> replacementRange{replacement};
    pyramid.replaceRange(5s, 5s, replacementRange);
    const auto updated = pyramid.view(0s, 16s, 4);
    VIDSCOPE_REQUIRE(updated.buckets[1].sampleCount == 4);
    VIDSCOPE_REQUIRE(std::abs(updated.buckets[1].averageMotion - 0.2925F) < 0.0001F);
}

VIDSCOPE_TEST(AnalysisPyramid_caps_long_video_storage_and_pixel_primitives)
{
    AnalysisPyramidConfig config;
    config.maximumBaseBuckets = 1'024;
    AnalysisPyramid pyramid(config);
    pyramid.reset(24h, 10'000'000);
    VIDSCOPE_REQUIRE(pyramid.baseBucketCount() <= config.maximumBaseBuckets);
    VIDSCOPE_REQUIRE(pyramid.levelCount() >= 6);

    std::vector<AnalysisSample> samples;
    samples.reserve(1'024);
    for (std::int64_t index = 0; index < 1'024; ++index) {
        samples.push_back(sample(index, std::chrono::seconds(index * 84)));
    }
    pyramid.rebuild(samples);

    const auto desktopWidthView = pyramid.view(0s, 24h, 320);
    VIDSCOPE_REQUIRE(desktopWidthView.level > 0);
    VIDSCOPE_REQUIRE(desktopWidthView.buckets.size() <= 320);
    const auto singlePixelView = pyramid.view(0s, 24h, 1);
    VIDSCOPE_REQUIRE(singlePixelView.buckets.size() <= 1);
}

VIDSCOPE_TEST(DetectionEngine_detects_scene_peaks_exact_near_and_freeze_ranges)
{
    std::vector<AnalysisSample> samples;
    for (std::int64_t index = 0; index < 10; ++index) {
        auto value = sample(index, std::chrono::milliseconds(index * 100));
        value.duration = 100ms;
        value.sceneScore = 0.05F;
        value.duplicateScore = 0.2F;
        value.contentHash = static_cast<std::uint64_t>(index + 1);
        value.perceptualHash = static_cast<std::uint64_t>(index + 1);
        samples.push_back(value);
    }
    samples[3].sceneScore = 0.85F;
    samples[4].sceneScore = 0.60F;
    for (std::size_t index = 0; index < 4; ++index) {
        samples[index].contentHash = 77U;
        samples[index].perceptualHash = 77U;
        if (index > 0) {
            samples[index].duplicateScore = 1.0F;
        }
    }
    samples[4].duplicateScore = 0.99F;
    samples[5].duplicateScore = 0.99F;
    samples[6].duplicateScore = 0.99F;

    DetectionConfig config;
    config.sceneThreshold = 0.5F;
    config.minimumSceneSeparation = 200ms;
    config.nearDuplicateThreshold = 0.98F;
    config.freezeThreshold = 0.995F;
    config.minimumFreezeDuration = 300ms;
    config.minimumFreezeFrames = 3;
    config.minimumRepeatedSeparation = 10s;
    const auto results = DetectionEngine::analyze(samples, config);

    VIDSCOPE_REQUIRE(results.scenes.size() == 2);
    VIDSCOPE_REQUIRE(results.scenes[1].firstFrame == 3);
    const auto exact = std::find_if(
        results.duplicates.cbegin(),
        results.duplicates.cend(),
        [](const auto& result) { return result.kind == DetectionKind::ExactDuplicate; });
    const auto near = std::find_if(
        results.duplicates.cbegin(),
        results.duplicates.cend(),
        [](const auto& result) { return result.kind == DetectionKind::NearDuplicate; });
    VIDSCOPE_REQUIRE(exact != results.duplicates.cend());
    VIDSCOPE_REQUIRE(exact->firstFrame == 0);
    VIDSCOPE_REQUIRE(exact->lastFrame == 3);
    VIDSCOPE_REQUIRE(near != results.duplicates.cend());
    VIDSCOPE_REQUIRE(near->firstFrame == 3);
    VIDSCOPE_REQUIRE(near->lastFrame == 6);
    VIDSCOPE_REQUIRE(results.freezes.size() == 1);
    VIDSCOPE_REQUIRE(results.freezes.front().frameCount == 4);
}

VIDSCOPE_TEST(DetectionEngine_finds_bounded_non_adjacent_repeated_sections)
{
    std::vector<AnalysisSample> samples;
    constexpr std::uint64_t hashes[] = {11, 22, 33, 90, 91, 92, 11, 22, 33};
    for (std::int64_t index = 0; index < 9; ++index) {
        auto value = sample(index, std::chrono::milliseconds(index * 100));
        value.duration = 100ms;
        value.sceneScore = 0.0F;
        value.duplicateScore = 0.1F;
        value.contentHash = hashes[index];
        value.perceptualHash = hashes[index];
        samples.push_back(value);
    }
    DetectionConfig config;
    config.sceneThreshold = 1.0F;
    config.minimumRepeatedFrames = 3;
    config.minimumRepeatedSeparation = 500ms;
    config.maximumResultsPerKind = 4;
    const auto results = DetectionEngine::analyze(samples, config);
    const auto repeated = std::find_if(
        results.duplicates.cbegin(),
        results.duplicates.cend(),
        [](const auto& result) { return result.kind == DetectionKind::RepeatedSection; });
    VIDSCOPE_REQUIRE(repeated != results.duplicates.cend());
    VIDSCOPE_REQUIRE(repeated->firstFrame == 6);
    VIDSCOPE_REQUIRE(repeated->lastFrame == 8);
    VIDSCOPE_REQUIRE(repeated->matchingFirstFrame == 0);
    VIDSCOPE_REQUIRE(repeated->matchingLastFrame == 2);
    VIDSCOPE_REQUIRE(results.duplicates.size() <= config.maximumResultsPerKind);
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
    samples.back().sceneScore = 0.75F;
    samples.back().duplicateScore = 0.25F;
    samples.back().contentHash = 0x1234U;
    samples.back().perceptualHash = 0x5678U;
    VIDSCOPE_REQUIRE(cache.save(info, samples, true));

    const auto restored = cache.load(info);
    VIDSCOPE_REQUIRE(restored.complete);
    VIDSCOPE_REQUIRE(restored.samples == samples);

    VIDSCOPE_REQUIRE(mediaFile.open(QIODevice::Append));
    VIDSCOPE_REQUIRE(mediaFile.write("changed", 7) == 7);
    mediaFile.close();
    VIDSCOPE_REQUIRE(cache.load(info).samples.empty());
}
