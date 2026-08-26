#include "analysis/VideoAnalyzer.h"

#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace vidscope::analysis {
namespace {

constexpr int kMaximumAnalysisDimension = 2'048;
constexpr std::size_t kHistogramBins = 32;

void validateComparable(const LumaPlane& previous, const LumaPlane& current)
{
    if (!previous.isValid() || !current.isValid()
        || previous.width != current.width || previous.height != current.height
        || previous.pixels.size() != current.pixels.size()) {
        throw std::invalid_argument("Analysis luma planes must have equal, valid geometry");
    }
}

[[nodiscard]] FrameAnalysisMetrics compareValidated(
    const LumaPlane& previous,
    const LumaPlane& current,
    const std::uint64_t previousPerceptualHash,
    const std::uint64_t currentPerceptualHash)
{
    std::array<std::uint64_t, kHistogramBins> previousHistogram{};
    std::array<std::uint64_t, kHistogramBins> currentHistogram{};
    std::uint64_t absoluteDifference = 0;
    for (std::size_t index = 0; index < current.pixels.size(); ++index) {
        const auto before = previous.pixels[index];
        const auto after = current.pixels[index];
        previousHistogram[static_cast<std::size_t>(before) * kHistogramBins / 256U]++;
        currentHistogram[static_cast<std::size_t>(after) * kHistogramBins / 256U]++;
        const int difference = static_cast<int>(after) - static_cast<int>(before);
        absoluteDifference += static_cast<std::uint64_t>(std::abs(difference));
    }

    std::uint64_t histogramIntersection = 0;
    for (std::size_t index = 0; index < kHistogramBins; ++index) {
        histogramIntersection += std::min(previousHistogram[index], currentHistogram[index]);
    }
    const double count = static_cast<double>(current.pixels.size());
    const double motion = std::clamp(
        static_cast<double>(absoluteDifference) / (255.0 * count),
        0.0,
        1.0);
    const double pixelSimilarity = 1.0 - motion;
    const double histogramSimilarity = std::clamp(
        static_cast<double>(histogramIntersection) / count,
        0.0,
        1.0);
    const double perceptualSimilarity = 1.0
        - static_cast<double>(std::popcount(
            previousPerceptualHash ^ currentPerceptualHash)) / 64.0;

    FrameAnalysisMetrics result;
    result.motion = static_cast<float>(motion);
    result.similarity = static_cast<float>(std::clamp(
        pixelSimilarity * 0.8 + histogramSimilarity * 0.2,
        0.0,
        1.0));
    result.sceneChange = static_cast<float>(std::clamp(
        motion * 0.65 + (1.0 - histogramSimilarity) * 0.35,
        0.0,
        1.0));
    result.duplicate = static_cast<float>(std::clamp(
        pixelSimilarity * 0.6
            + histogramSimilarity * 0.2
            + std::min(pixelSimilarity, perceptualSimilarity) * 0.2,
        0.0,
        1.0));
    return result;
}

} // namespace

bool LumaPlane::isValid() const noexcept
{
    if (width <= 0 || height <= 0) {
        return false;
    }
    const auto expected = static_cast<std::uint64_t>(width)
        * static_cast<std::uint64_t>(height);
    return expected == pixels.size();
}

class LumaExtractor::Impl final {
public:
    explicit Impl(QSize requestedSize)
        : size(std::move(requestedSize))
    {
        if (!size.isValid() || size.width() <= 0 || size.height() <= 0
            || size.width() > kMaximumAnalysisDimension
            || size.height() > kMaximumAnalysisDimension) {
            throw std::invalid_argument("The analysis luma size is invalid or unreasonably large");
        }
    }

    QSize size;
    media::SwsContextPtr context;
};

LumaExtractor::LumaExtractor(QSize outputSize)
    : impl_(std::make_unique<Impl>(std::move(outputSize)))
{
}

LumaExtractor::~LumaExtractor() = default;

LumaPlane LumaExtractor::extract(
    const media::DecodedFrame& frame,
    const core::CancellationToken cancellation)
{
    LumaPlane result;
    if (!extract(frame, result, cancellation)) {
        return {};
    }
    return result;
}

bool LumaExtractor::extract(
    const media::DecodedFrame& frame,
    LumaPlane& destination,
    const core::CancellationToken cancellation)
{
    if (cancellation.isCancellationRequested()) {
        return false;
    }
    if (!frame.storage || frame.storage->get() == nullptr) {
        throw std::invalid_argument("The decoded frame has no retained image storage");
    }
    const AVFrame* source = frame.storage->get();
    if (source->width <= 0 || source->height <= 0 || source->format < 0
        || source->data[0] == nullptr) {
        throw std::invalid_argument("The decoded frame cannot be converted to luma");
    }
    const auto sourceFormat = static_cast<AVPixelFormat>(source->format);
    const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(sourceFormat);
    if (descriptor != nullptr && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0) {
        throw std::invalid_argument("Hardware frames must be transferred before luma extraction");
    }

    SwsContext* previousContext = impl_->context.release();
    SwsContext* context = sws_getCachedContext(
        previousContext,
        source->width,
        source->height,
        sourceFormat,
        impl_->size.width(),
        impl_->size.height(),
        AV_PIX_FMT_GRAY8,
        SWS_AREA,
        nullptr,
        nullptr,
        nullptr);
    impl_->context.reset(context);
    if (context == nullptr) {
        throw std::runtime_error("FFmpeg could not initialize luma extraction");
    }

    destination.width = impl_->size.width();
    destination.height = impl_->size.height();
    const auto pixelCount = static_cast<std::size_t>(destination.width)
        * static_cast<std::size_t>(destination.height);
    destination.pixels.resize(pixelCount);
    if (cancellation.isCancellationRequested()) {
        return false;
    }

    std::array<std::uint8_t*, 4> outputData{
        destination.pixels.data(), nullptr, nullptr, nullptr};
    std::array<int, 4> strides{destination.width, 0, 0, 0};
    const int rows = sws_scale(
        context,
        source->data,
        source->linesize,
        0,
        source->height,
        outputData.data(),
        strides.data());
    if (rows != destination.height) {
        throw std::runtime_error("FFmpeg did not extract the complete luma plane");
    }
    if (cancellation.isCancellationRequested()) {
        return false;
    }
    return true;
}

void LumaExtractor::reset() noexcept
{
    impl_->context.reset();
}

QSize LumaExtractor::outputSize() const noexcept
{
    return impl_->size;
}

float VideoAnalyzer::motionScore(const LumaPlane& previous, const LumaPlane& current)
{
    return compare(previous, current).motion;
}

FrameAnalysisMetrics VideoAnalyzer::compare(
    const LumaPlane& previous,
    const LumaPlane& current)
{
    validateComparable(previous, current);
    return compareValidated(
        previous,
        current,
        perceptualHash(previous),
        perceptualHash(current));
}

FrameAnalysisMetrics VideoAnalyzer::compare(
    const LumaPlane& previous,
    const LumaPlane& current,
    const std::uint64_t previousPerceptualHash,
    const std::uint64_t currentPerceptualHash)
{
    validateComparable(previous, current);
    return compareValidated(
        previous,
        current,
        previousPerceptualHash,
        currentPerceptualHash);
}

float VideoAnalyzer::similarityScore(const LumaPlane& previous, const LumaPlane& current)
{
    return compare(previous, current).similarity;
}

std::uint64_t VideoAnalyzer::contentHash(const LumaPlane& plane)
{
    if (!plane.isValid()) {
        throw std::invalid_argument("Cannot hash an invalid analysis luma plane");
    }
    constexpr std::uint64_t offset = 14'695'981'039'346'656'037ULL;
    constexpr std::uint64_t prime = 1'099'511'628'211ULL;
    std::uint64_t hash = offset;
    const auto mix = [&](const std::uint8_t value) {
        hash ^= value;
        hash *= prime;
    };
    for (const auto pixel : plane.pixels) {
        mix(pixel);
    }
    for (int shift = 0; shift < 32; shift += 8) {
        mix(static_cast<std::uint8_t>(
            static_cast<std::uint32_t>(plane.width) >> static_cast<unsigned>(shift)));
        mix(static_cast<std::uint8_t>(
            static_cast<std::uint32_t>(plane.height) >> static_cast<unsigned>(shift)));
    }
    return hash;
}

std::uint64_t VideoAnalyzer::perceptualHash(const LumaPlane& plane)
{
    if (!plane.isValid()) {
        throw std::invalid_argument("Cannot hash an invalid analysis luma plane");
    }
    std::array<std::uint32_t, 64> blocks{};
    for (int blockY = 0; blockY < 8; ++blockY) {
        const int yStart = blockY * plane.height / 8;
        const int yEnd = std::max(yStart + 1, (blockY + 1) * plane.height / 8);
        for (int blockX = 0; blockX < 8; ++blockX) {
            const int xStart = blockX * plane.width / 8;
            const int xEnd = std::max(xStart + 1, (blockX + 1) * plane.width / 8);
            std::uint64_t total = 0;
            std::uint64_t count = 0;
            for (int y = yStart; y < std::min(yEnd, plane.height); ++y) {
                for (int x = xStart; x < std::min(xEnd, plane.width); ++x) {
                    total += plane.pixels[
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(plane.width)
                        + static_cast<std::size_t>(x)];
                    ++count;
                }
            }
            blocks[static_cast<std::size_t>(blockY * 8 + blockX)] =
                static_cast<std::uint32_t>(total / std::max<std::uint64_t>(1, count));
        }
    }

    std::uint64_t hash = 0;
    std::size_t bit = 0;
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 7; ++x) {
            const auto left = blocks[static_cast<std::size_t>(y * 8 + x)];
            const auto right = blocks[static_cast<std::size_t>(y * 8 + x + 1)];
            if (left >= right) {
                hash |= std::uint64_t{1} << static_cast<unsigned>(bit);
            }
            ++bit;
        }
    }
    const std::uint64_t total = std::accumulate(
        blocks.cbegin(),
        blocks.cend(),
        std::uint64_t{0});
    const auto mean = static_cast<std::uint8_t>(total / blocks.size());
    hash |= static_cast<std::uint64_t>(mean) << 56U;
    return hash;
}

} // namespace vidscope::analysis
