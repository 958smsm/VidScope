#pragma once

#include "analysis/AnalysisTypes.h"

#include <QtCore/QMetaType>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace vidscope::analysis {

enum class DetectionKind : std::uint8_t {
    SceneChange,
    ExactDuplicate,
    NearDuplicate,
    RepeatedSection,
    Freeze,
};

struct DetectionResult final {
    DetectionKind kind = DetectionKind::SceneChange;
    media::MediaTime start{};
    media::MediaTime end{};
    std::int64_t firstFrame = -1;
    std::int64_t lastFrame = -1;
    std::size_t frameCount = 0;
    float score = 0.0F;
    std::optional<media::MediaTime> matchingStart;
    std::optional<media::MediaTime> matchingEnd;
    std::int64_t matchingFirstFrame = -1;
    std::int64_t matchingLastFrame = -1;

    friend bool operator==(const DetectionResult&, const DetectionResult&) = default;
};

struct DetectionResults final {
    std::vector<DetectionResult> scenes;
    std::vector<DetectionResult> duplicates;
    std::vector<DetectionResult> freezes;
    std::size_t analyzedSamples = 0;

    friend bool operator==(const DetectionResults&, const DetectionResults&) = default;
};

struct DetectionConfig final {
    float sceneThreshold = 0.45F;
    media::MediaTime minimumSceneSeparation = std::chrono::milliseconds(250);
    float nearDuplicateThreshold = 0.985F;
    float freezeThreshold = 0.995F;
    media::MediaTime minimumFreezeDuration = std::chrono::milliseconds(500);
    std::size_t minimumDuplicateFrames = 2;
    std::size_t minimumFreezeFrames = 3;
    std::size_t minimumRepeatedFrames = 3;
    media::MediaTime minimumRepeatedSeparation = std::chrono::seconds(2);
    unsigned maximumPerceptualDistance = 4;
    std::size_t maximumFingerprintCandidates = 8;
    std::size_t maximumResultsPerKind = 10'000;

    friend bool operator==(const DetectionConfig&, const DetectionConfig&) = default;
};

class DetectionEngine final {
public:
    [[nodiscard]] static DetectionConfig normalized(DetectionConfig config) noexcept;
    [[nodiscard]] static DetectionResults analyze(
        std::span<const AnalysisSample> samples,
        DetectionConfig config = {});
};

} // namespace vidscope::analysis

Q_DECLARE_METATYPE(vidscope::analysis::DetectionConfig)
Q_DECLARE_METATYPE(vidscope::analysis::DetectionKind)
