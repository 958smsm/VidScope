#include "analysis/VideoAnalyzer.h"

#include <array>
#include <cmath>
#include <limits>
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
    if (cancellation.isCancellationRequested()) {
        return {};
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

    LumaPlane result;
    result.width = impl_->size.width();
    result.height = impl_->size.height();
    const auto pixelCount = static_cast<std::size_t>(result.width)
        * static_cast<std::size_t>(result.height);
    result.pixels.resize(pixelCount);
    if (cancellation.isCancellationRequested()) {
        return {};
    }

    std::array<std::uint8_t*, 4> destination{result.pixels.data(), nullptr, nullptr, nullptr};
    std::array<int, 4> strides{result.width, 0, 0, 0};
    const int rows = sws_scale(
        context,
        source->data,
        source->linesize,
        0,
        source->height,
        destination.data(),
        strides.data());
    if (rows != result.height) {
        throw std::runtime_error("FFmpeg did not extract the complete luma plane");
    }
    if (cancellation.isCancellationRequested()) {
        return {};
    }
    return result;
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
    validateComparable(previous, current);
    std::uint64_t absoluteDifference = 0;
    for (std::size_t index = 0; index < current.pixels.size(); ++index) {
        const int difference = static_cast<int>(current.pixels[index])
            - static_cast<int>(previous.pixels[index]);
        absoluteDifference += static_cast<std::uint64_t>(std::abs(difference));
    }
    const double denominator = 255.0 * static_cast<double>(current.pixels.size());
    return static_cast<float>(std::clamp(
        static_cast<double>(absoluteDifference) / denominator,
        0.0,
        1.0));
}

float VideoAnalyzer::similarityScore(const LumaPlane& previous, const LumaPlane& current)
{
    validateComparable(previous, current);
    std::array<std::uint64_t, kHistogramBins> previousHistogram{};
    std::array<std::uint64_t, kHistogramBins> currentHistogram{};
    std::uint64_t absoluteDifference = 0;

    for (std::size_t index = 0; index < current.pixels.size(); ++index) {
        const auto before = previous.pixels[index];
        const auto after = current.pixels[index];
        previousHistogram[static_cast<std::size_t>(before) * kHistogramBins / 256U]++;
        currentHistogram[static_cast<std::size_t>(after) * kHistogramBins / 256U]++;
        absoluteDifference += static_cast<std::uint64_t>(
            std::abs(static_cast<int>(after) - static_cast<int>(before)));
    }

    std::uint64_t histogramIntersection = 0;
    for (std::size_t index = 0; index < kHistogramBins; ++index) {
        histogramIntersection += std::min(previousHistogram[index], currentHistogram[index]);
    }
    const double count = static_cast<double>(current.pixels.size());
    const double pixelSimilarity = 1.0
        - static_cast<double>(absoluteDifference) / (255.0 * count);
    const double histogramSimilarity = static_cast<double>(histogramIntersection) / count;
    return static_cast<float>(std::clamp(
        pixelSimilarity * 0.8 + histogramSimilarity * 0.2,
        0.0,
        1.0));
}

} // namespace vidscope::analysis

