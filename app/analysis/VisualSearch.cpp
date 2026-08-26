#include "analysis/VisualSearch.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace vidscope::analysis {
namespace {

[[nodiscard]] media::MediaTime absoluteDistance(
    const media::MediaTime left,
    const media::MediaTime right) noexcept
{
    return left >= right ? left - right : right - left;
}

} // namespace

std::vector<VisualMatch> VisualSearch::findSimilar(
    const std::span<const AnalysisSample> samples,
    const std::uint64_t queryHash,
    const media::MediaTime queryTime,
    const std::int64_t queryPresentationIndex,
    const std::size_t maximumResults,
    const std::uint8_t maximumHammingDistance)
{
    std::vector<VisualMatch> matches;
    if (maximumResults == 0) {
        return matches;
    }
    matches.reserve(std::min(samples.size(), maximumResults));
    for (const auto& sample : samples) {
        if (!sample.perceptualHash) {
            continue;
        }
        const bool sameFrame = queryPresentationIndex >= 0
            && sample.presentationIndex >= 0
            ? sample.presentationIndex == queryPresentationIndex
            : sample.presentationTime == queryTime;
        if (sameFrame) {
            continue;
        }
        const auto distance = static_cast<std::uint8_t>(
            std::popcount(*sample.perceptualHash ^ queryHash));
        if (distance > maximumHammingDistance) {
            continue;
        }
        matches.push_back({
            sample.presentationTime,
            sample.presentationIndex,
            distance,
            1.0F - static_cast<float>(distance) / 64.0F,
        });
    }
    std::sort(
        matches.begin(),
        matches.end(),
        [queryTime](const VisualMatch& left, const VisualMatch& right) {
            if (left.hammingDistance != right.hammingDistance) {
                return left.hammingDistance < right.hammingDistance;
            }
            const auto leftDistance = absoluteDistance(left.presentationTime, queryTime);
            const auto rightDistance = absoluteDistance(right.presentationTime, queryTime);
            if (leftDistance != rightDistance) {
                return leftDistance < rightDistance;
            }
            return left.presentationTime < right.presentationTime;
        });
    if (matches.size() > maximumResults) {
        matches.resize(maximumResults);
    }
    return matches;
}

} // namespace vidscope::analysis
