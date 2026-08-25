#include "analysis/DetectionEngine.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <limits>
#include <unordered_map>
#include <utility>

namespace vidscope::analysis {
namespace {

[[nodiscard]] media::MediaTime frameEnd(const AnalysisSample& sample) noexcept
{
    return sample.presentationTime
        + std::max(sample.duration, std::chrono::nanoseconds(1));
}

[[nodiscard]] bool exactTransition(
    const AnalysisSample& previous,
    const AnalysisSample& current) noexcept
{
    return previous.contentHash && current.contentHash
        && *previous.contentHash == *current.contentHash;
}

[[nodiscard]] DetectionResult makeRange(
    const DetectionKind kind,
    const AnalysisSample& first,
    const AnalysisSample& last,
    const std::size_t frameCount,
    const float score)
{
    DetectionResult result;
    result.kind = kind;
    result.start = first.presentationTime;
    result.end = std::max(first.presentationTime, frameEnd(last));
    result.firstFrame = first.presentationIndex;
    result.lastFrame = last.presentationIndex;
    result.frameCount = frameCount;
    result.score = std::clamp(score, 0.0F, 1.0F);
    return result;
}

void detectScenes(
    const std::span<const AnalysisSample> samples,
    const DetectionConfig& config,
    DetectionResults& results)
{
    if (samples.empty() || config.maximumResultsPerKind == 0) {
        return;
    }
    DetectionResult start = makeRange(
        DetectionKind::SceneChange,
        samples.front(),
        samples.front(),
        1,
        1.0F);
    results.scenes.push_back(start);

    for (std::size_t index = 1;
         index < samples.size() && results.scenes.size() < config.maximumResultsPerKind;
         ++index) {
        const auto score = samples[index].sceneScore;
        if (!score || *score < config.sceneThreshold) {
            continue;
        }
        const float previousScore = index > 1
            ? samples[index - 1].sceneScore.value_or(0.0F)
            : 0.0F;
        const float nextScore = index + 1 < samples.size()
            ? samples[index + 1].sceneScore.value_or(0.0F)
            : 0.0F;
        if (*score < previousScore || *score < nextScore) {
            continue;
        }

        DetectionResult candidate = makeRange(
            DetectionKind::SceneChange,
            samples[index],
            samples[index],
            1,
            *score);
        auto& latest = results.scenes.back();
        if (candidate.start - latest.start < config.minimumSceneSeparation) {
            if (latest.kind == DetectionKind::SceneChange && latest.score < candidate.score
                && latest.start != samples.front().presentationTime) {
                latest = candidate;
            }
            continue;
        }
        results.scenes.push_back(candidate);
    }
}

void detectAdjacentDuplicatesAndFreezes(
    const std::span<const AnalysisSample> samples,
    const DetectionConfig& config,
    DetectionResults& results)
{
    std::size_t index = 1;
    while (index < samples.size()
           && results.duplicates.size() < config.maximumResultsPerKind) {
        if (!samples[index].duplicateScore
            || *samples[index].duplicateScore < config.nearDuplicateThreshold) {
            ++index;
            continue;
        }
        const std::size_t firstTransition = index;
        const bool exact = exactTransition(samples[index - 1], samples[index]);
        double totalScore = 0.0;
        std::size_t transitions = 0;
        while (index < samples.size() && samples[index].duplicateScore
               && *samples[index].duplicateScore >= config.nearDuplicateThreshold
               && exactTransition(samples[index - 1], samples[index]) == exact) {
            totalScore += *samples[index].duplicateScore;
            ++transitions;
            ++index;
        }
        const std::size_t firstFrame = firstTransition - 1;
        const std::size_t lastFrame = index - 1;
        const std::size_t frameCount = lastFrame - firstFrame + 1;
        if (frameCount >= config.minimumDuplicateFrames) {
            results.duplicates.push_back(makeRange(
                exact ? DetectionKind::ExactDuplicate : DetectionKind::NearDuplicate,
                samples[firstFrame],
                samples[lastFrame],
                frameCount,
                static_cast<float>(totalScore / static_cast<double>(transitions))));
        }
    }

    index = 1;
    while (index < samples.size() && results.freezes.size() < config.maximumResultsPerKind) {
        if (!samples[index].duplicateScore
            || *samples[index].duplicateScore < config.freezeThreshold) {
            ++index;
            continue;
        }
        const std::size_t firstTransition = index;
        double totalScore = 0.0;
        std::size_t transitions = 0;
        while (index < samples.size() && samples[index].duplicateScore
               && *samples[index].duplicateScore >= config.freezeThreshold) {
            totalScore += *samples[index].duplicateScore;
            ++transitions;
            ++index;
        }
        const std::size_t firstFrame = firstTransition - 1;
        const std::size_t lastFrame = index - 1;
        const std::size_t frameCount = lastFrame - firstFrame + 1;
        const auto duration = frameEnd(samples[lastFrame]) - samples[firstFrame].presentationTime;
        if (frameCount >= config.minimumFreezeFrames
            && duration >= config.minimumFreezeDuration) {
            results.freezes.push_back(makeRange(
                DetectionKind::Freeze,
                samples[firstFrame],
                samples[lastFrame],
                frameCount,
                static_cast<float>(totalScore / static_cast<double>(transitions))));
        }
    }
}

void detectRepeatedSections(
    const std::span<const AnalysisSample> samples,
    const DetectionConfig& config,
    DetectionResults& results)
{
    if (config.minimumRepeatedFrames == 0 || config.maximumFingerprintCandidates == 0
        || results.duplicates.size() >= config.maximumResultsPerKind) {
        return;
    }
    std::unordered_map<std::uint64_t, std::deque<std::size_t>> positions;
    positions.reserve(std::min<std::size_t>(samples.size(), 65'536));
    std::size_t coveredUntil = 0;

    for (std::size_t current = 0;
         current < samples.size() && results.duplicates.size() < config.maximumResultsPerKind;
         ++current) {
        if (!samples[current].perceptualHash) {
            continue;
        }
        const auto hash = *samples[current].perceptualHash;
        auto& candidates = positions[hash];
        std::optional<DetectionResult> best;

        if (current >= coveredUntil) {
            for (auto candidate = candidates.rbegin(); candidate != candidates.rend(); ++candidate) {
                const std::size_t previous = *candidate;
                if (current <= previous
                    || samples[current].presentationTime - samples[previous].presentationTime
                        < config.minimumRepeatedSeparation) {
                    continue;
                }
                std::size_t length = 0;
                double similarityTotal = 0.0;
                while (current + length < samples.size()
                       && previous + length < current
                       && samples[current + length].perceptualHash
                       && samples[previous + length].perceptualHash) {
                    const unsigned distance = std::popcount(
                        *samples[current + length].perceptualHash
                        ^ *samples[previous + length].perceptualHash);
                    if (distance > config.maximumPerceptualDistance) {
                        break;
                    }
                    similarityTotal += 1.0 - static_cast<double>(distance) / 64.0;
                    ++length;
                }
                if (length < config.minimumRepeatedFrames) {
                    continue;
                }
                DetectionResult result = makeRange(
                    DetectionKind::RepeatedSection,
                    samples[current],
                    samples[current + length - 1],
                    length,
                    static_cast<float>(similarityTotal / static_cast<double>(length)));
                result.matchingStart = samples[previous].presentationTime;
                result.matchingEnd = frameEnd(samples[previous + length - 1]);
                result.matchingFirstFrame = samples[previous].presentationIndex;
                result.matchingLastFrame = samples[previous + length - 1].presentationIndex;
                if (!best || result.frameCount > best->frameCount
                    || (result.frameCount == best->frameCount && result.score > best->score)) {
                    best = result;
                }
            }
        }

        candidates.push_back(current);
        while (candidates.size() > config.maximumFingerprintCandidates) {
            candidates.pop_front();
        }
        if (best) {
            coveredUntil = current + best->frameCount;
            results.duplicates.push_back(*best);
        }
    }
}

} // namespace

DetectionConfig DetectionEngine::normalized(DetectionConfig config) noexcept
{
    config.sceneThreshold = std::clamp(config.sceneThreshold, 0.0F, 1.0F);
    config.nearDuplicateThreshold = std::clamp(
        config.nearDuplicateThreshold,
        0.0F,
        1.0F);
    config.freezeThreshold = std::clamp(config.freezeThreshold, 0.0F, 1.0F);
    config.minimumSceneSeparation = std::max(
        config.minimumSceneSeparation,
        media::MediaTime::zero());
    config.minimumFreezeDuration = std::max(
        config.minimumFreezeDuration,
        media::MediaTime::zero());
    config.minimumRepeatedSeparation = std::max(
        config.minimumRepeatedSeparation,
        media::MediaTime::zero());
    config.minimumDuplicateFrames = std::max<std::size_t>(2, config.minimumDuplicateFrames);
    config.minimumFreezeFrames = std::max<std::size_t>(2, config.minimumFreezeFrames);
    config.minimumRepeatedFrames = std::max<std::size_t>(2, config.minimumRepeatedFrames);
    config.maximumPerceptualDistance = std::min(config.maximumPerceptualDistance, 64U);
    config.maximumFingerprintCandidates = std::clamp<std::size_t>(
        config.maximumFingerprintCandidates,
        1,
        64);
    config.maximumResultsPerKind = std::min<std::size_t>(
        config.maximumResultsPerKind,
        100'000);
    return config;
}

DetectionResults DetectionEngine::analyze(
    const std::span<const AnalysisSample> samples,
    DetectionConfig config)
{
    config = normalized(std::move(config));
    DetectionResults results;
    results.analyzedSamples = samples.size();
    detectScenes(samples, config, results);
    detectAdjacentDuplicatesAndFreezes(samples, config, results);
    detectRepeatedSections(samples, config, results);
    return results;
}

} // namespace vidscope::analysis
