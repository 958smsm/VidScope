#pragma once

#include "core/Cancellation.h"
#include "media/FfmpegRaii.h"
#include "media/MediaTypes.h"

#include <QtCore/QSize>

#include <cstdint>
#include <memory>
#include <vector>

namespace vidscope::analysis {

struct LumaPlane final {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;

    [[nodiscard]] bool isValid() const noexcept;
};

struct FrameAnalysisMetrics final {
    float motion = 0.0F;
    float similarity = 0.0F;
    float sceneChange = 0.0F;
    float duplicate = 0.0F;
};

class LumaExtractor final {
public:
    explicit LumaExtractor(QSize outputSize = QSize(160, 90));
    ~LumaExtractor();
    LumaExtractor(const LumaExtractor&) = delete;
    LumaExtractor& operator=(const LumaExtractor&) = delete;

    [[nodiscard]] LumaPlane extract(
        const media::DecodedFrame& frame,
        core::CancellationToken cancellation = {});
    void reset() noexcept;
    [[nodiscard]] QSize outputSize() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class VideoAnalyzer final {
public:
    [[nodiscard]] static FrameAnalysisMetrics compare(
        const LumaPlane& previous,
        const LumaPlane& current);
    [[nodiscard]] static float motionScore(
        const LumaPlane& previous,
        const LumaPlane& current);
    [[nodiscard]] static float similarityScore(
        const LumaPlane& previous,
        const LumaPlane& current);
    [[nodiscard]] static std::uint64_t contentHash(const LumaPlane& plane);
    [[nodiscard]] static std::uint64_t perceptualHash(const LumaPlane& plane);
};

} // namespace vidscope::analysis
