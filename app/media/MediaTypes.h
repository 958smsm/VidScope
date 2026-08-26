#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

namespace vidscope::media {

using MediaTime = std::chrono::nanoseconds;
inline constexpr MediaTime kNoMediaTime = MediaTime::min();

class FrameStorage final {
public:
    explicit FrameStorage(const AVFrame* source);
    ~FrameStorage();

    FrameStorage(const FrameStorage&) = delete;
    FrameStorage& operator=(const FrameStorage&) = delete;
    FrameStorage(FrameStorage&&) = delete;
    FrameStorage& operator=(FrameStorage&&) = delete;

    [[nodiscard]] const AVFrame* get() const noexcept { return frame_; }
    [[nodiscard]] std::size_t estimatedBytes() const noexcept { return estimatedBytes_; }

private:
    AVFrame* frame_ = nullptr;
    std::size_t estimatedBytes_ = 0;
};

struct FrameId final {
    std::int64_t presentationIndex = -1;
    std::int64_t pts = AV_NOPTS_VALUE;
    std::uint64_t sessionSerial = 0;

    friend bool operator==(const FrameId&, const FrameId&) = default;
};

struct ChromaticityPoint final {
    double x = 0.0;
    double y = 0.0;

    friend bool operator==(const ChromaticityPoint&, const ChromaticityPoint&) = default;
};

struct MasteringDisplayPrimaries final {
    ChromaticityPoint red;
    ChromaticityPoint green;
    ChromaticityPoint blue;
    ChromaticityPoint whitePoint;

    friend bool operator==(
        const MasteringDisplayPrimaries&,
        const MasteringDisplayPrimaries&) = default;
};

struct MasteringDisplayLuminance final {
    double minimumNits = 0.0;
    double maximumNits = 0.0;

    friend bool operator==(
        const MasteringDisplayLuminance&,
        const MasteringDisplayLuminance&) = default;
};

struct MasteringDisplayMetadata final {
    std::optional<MasteringDisplayPrimaries> primaries;
    std::optional<MasteringDisplayLuminance> luminance;

    friend bool operator==(
        const MasteringDisplayMetadata&,
        const MasteringDisplayMetadata&) = default;
};

struct ContentLightMetadata final {
    std::uint32_t maxContentLightLevel = 0; // MaxCLL, cd/m^2.
    std::uint32_t maxFrameAverageLightLevel = 0; // MaxFALL, cd/m^2.

    friend bool operator==(const ContentLightMetadata&, const ContentLightMetadata&) = default;
};

struct MediaChapter final {
    std::int64_t id = 0;
    MediaTime start{};
    MediaTime end{};
    std::string title;

    friend bool operator==(const MediaChapter&, const MediaChapter&) = default;
};

struct DecodedFrame final {
    std::shared_ptr<const FrameStorage> storage;
    FrameId id;

    std::int64_t dts = AV_NOPTS_VALUE;
    std::int64_t bestEffortTimestamp = AV_NOPTS_VALUE;
    AVRational timeBase{0, 1};
    MediaTime presentationTime{};
    MediaTime duration{};

    bool keyFrame = false;
    AVPictureType pictureType = AV_PICTURE_TYPE_NONE;
    int codedPictureNumber = -1;
    int displayPictureNumber = -1;

    int width = 0;
    int height = 0;
    AVPixelFormat pixelFormat = AV_PIX_FMT_NONE;
    AVColorRange colorRange = AVCOL_RANGE_UNSPECIFIED;
    AVColorSpace colorSpace = AVCOL_SPC_UNSPECIFIED;
    AVColorPrimaries colorPrimaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic colorTransfer = AVCOL_TRC_UNSPECIFIED;
    AVChromaLocation chromaLocation = AVCHROMA_LOC_UNSPECIFIED;
    int bitDepth = 0;
    std::optional<MasteringDisplayMetadata> masteringDisplay;
    std::optional<ContentLightMetadata> contentLight;

    [[nodiscard]] std::size_t estimatedBytes() const noexcept
    {
        return storage ? storage->estimatedBytes() : 0;
    }
};

using DecodedFramePtr = std::shared_ptr<const DecodedFrame>;

// Compares only visible image bytes, excluding per-row padding.
[[nodiscard]] bool visibleImagesEqual(
    const DecodedFrame& left,
    const DecodedFrame& right) noexcept;

[[nodiscard]] std::optional<MasteringDisplayMetadata> extractMasteringDisplayMetadata(
    const AVFrame& frame) noexcept;
[[nodiscard]] std::optional<ContentLightMetadata> extractContentLightMetadata(
    const AVFrame& frame) noexcept;

struct MediaInfo final {
    std::filesystem::path path;
    std::string containerName;
    std::string containerLongName;
    std::string codecName;
    std::string codecLongName;

    int videoStreamIndex = -1;
    int width = 0;
    int height = 0;
    AVPixelFormat pixelFormat = AV_PIX_FMT_NONE;
    AVRational timeBase{0, 1};
    AVRational averageFrameRate{0, 1};
    AVRational realFrameRate{0, 1};
    std::int64_t streamStartTimestamp = 0;
    MediaTime duration{};
    std::int64_t declaredFrameCount = 0;
    int codecDelayFrames = 0;
    int bitDepth = 0;
    AVColorRange colorRange = AVCOL_RANGE_UNSPECIFIED;
    AVColorSpace colorSpace = AVCOL_SPC_UNSPECIFIED;
    AVColorPrimaries colorPrimaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic colorTransfer = AVCOL_TRC_UNSPECIFIED;
    std::vector<MediaChapter> chapters;
};

using MediaInfoPtr = std::shared_ptr<const MediaInfo>;

[[nodiscard]] MediaTime timestampToMediaTime(
    std::int64_t timestamp,
    std::int64_t originTimestamp,
    AVRational timeBase) noexcept;

[[nodiscard]] std::int64_t mediaTimeToTimestamp(
    MediaTime time,
    std::int64_t originTimestamp,
    AVRational timeBase) noexcept;

[[nodiscard]] MediaTime nominalFrameDuration(AVRational frameRate) noexcept;
[[nodiscard]] const char* pictureTypeName(AVPictureType type) noexcept;

} // namespace vidscope::media
