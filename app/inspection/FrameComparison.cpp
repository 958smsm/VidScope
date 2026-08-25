#include "inspection/FrameComparison.h"

#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtGui/QColor>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

namespace vidscope::inspection {
namespace {

constexpr int kSsimBlockSize = 8;
constexpr double kSsimC1 = 6.5025; // (0.01 * 255)^2
constexpr double kSsimC2 = 58.5225; // (0.03 * 255)^2

[[nodiscard]] bool needsDifferenceImage(const ComparisonMode mode) noexcept
{
    return mode == ComparisonMode::AbsoluteDifference
        || mode == ComparisonMode::AmplifiedDifference;
}

[[nodiscard]] double luma(const uchar* pixel) noexcept
{
    return 0.2126 * static_cast<double>(pixel[0])
        + 0.7152 * static_cast<double>(pixel[1])
        + 0.0722 * static_cast<double>(pixel[2]);
}

[[nodiscard]] QRgb ssimColor(const double score) noexcept
{
    const double normalized = std::clamp(score, 0.0, 1.0);
    const int red = static_cast<int>(std::lround(255.0 * (1.0 - normalized)));
    const int green = static_cast<int>(std::lround(255.0 * normalized));
    const int blue = static_cast<int>(std::lround(40.0 + 80.0 * normalized));
    return qRgb(red, green, blue);
}

} // namespace

ComparisonResult FrameComparison::analyze(
    const QImage& frameA,
    const QImage& frameB,
    const ComparisonMode mode,
    const int differenceAmplification,
    const core::CancellationToken cancellation)
{
    ComparisonResult result;
    result.mode = mode;
    if (frameA.isNull() || frameB.isNull()) {
        result.metrics.detail = QStringLiteral("Set both frame A and frame B.");
        return result;
    }
    if (frameA.size() != frameB.size()) {
        result.metrics.detail = QStringLiteral(
            "Difference metrics require matching frame dimensions (%1x%2 vs %3x%4).")
            .arg(frameA.width())
            .arg(frameA.height())
            .arg(frameB.width())
            .arg(frameB.height());
        return result;
    }
    if (frameA.width() <= 0 || frameA.height() <= 0) {
        result.metrics.detail = QStringLiteral("The comparison frame dimensions are invalid.");
        return result;
    }
    if (cancellation.isCancellationRequested()) {
        result.cancelled = true;
        return result;
    }

    const QImage a = frameA.convertToFormat(QImage::Format_RGBA8888);
    const QImage b = frameB.convertToFormat(QImage::Format_RGBA8888);
    if (a.isNull() || b.isNull() || cancellation.isCancellationRequested()) {
        result.cancelled = cancellation.isCancellationRequested();
        if (!result.cancelled) {
            result.metrics.detail = QStringLiteral("Could not normalize comparison images.");
        }
        return result;
    }

    const bool makeDifference = needsDifferenceImage(mode);
    const bool makeSsimMap = mode == ComparisonMode::SsimMap;
    if (makeDifference || makeSsimMap) {
        result.visualization = QImage(a.size(), QImage::Format_RGB32);
        if (result.visualization.isNull()) {
            result.metrics.detail = QStringLiteral("Could not allocate the comparison surface.");
            return result;
        }
    }

    const int amplification = std::clamp(differenceAmplification, 1, 16);
    double squaredError = 0.0;
    for (int y = 0; y < a.height(); ++y) {
        if (cancellation.isCancellationRequested()) {
            result.cancelled = true;
            result.visualization = {};
            return result;
        }
        const uchar* rowA = a.constScanLine(y);
        const uchar* rowB = b.constScanLine(y);
        auto* output = makeDifference
            ? reinterpret_cast<QRgb*>(result.visualization.scanLine(y))
            : nullptr;
        for (int x = 0; x < a.width(); ++x) {
            const int offset = x * 4;
            const int red = std::abs(
                static_cast<int>(rowA[offset]) - static_cast<int>(rowB[offset]));
            const int green = std::abs(
                static_cast<int>(rowA[offset + 1]) - static_cast<int>(rowB[offset + 1]));
            const int blue = std::abs(
                static_cast<int>(rowA[offset + 2]) - static_cast<int>(rowB[offset + 2]));
            squaredError += static_cast<double>(red * red + green * green + blue * blue);
            if (output != nullptr) {
                const int scale = mode == ComparisonMode::AmplifiedDifference
                    ? amplification
                    : 1;
                output[x] = qRgb(
                    std::min(255, red * scale),
                    std::min(255, green * scale),
                    std::min(255, blue * scale));
            }
        }
    }

    double weightedSsim = 0.0;
    std::uint64_t ssimPixels = 0;
    for (int blockY = 0; blockY < a.height(); blockY += kSsimBlockSize) {
        if (cancellation.isCancellationRequested()) {
            result.cancelled = true;
            result.visualization = {};
            return result;
        }
        const int endY = std::min(blockY + kSsimBlockSize, a.height());
        for (int blockX = 0; blockX < a.width(); blockX += kSsimBlockSize) {
            const int endX = std::min(blockX + kSsimBlockSize, a.width());
            double sumA = 0.0;
            double sumB = 0.0;
            double squareA = 0.0;
            double squareB = 0.0;
            double product = 0.0;
            std::uint64_t count = 0;
            for (int y = blockY; y < endY; ++y) {
                const uchar* rowA = a.constScanLine(y);
                const uchar* rowB = b.constScanLine(y);
                for (int x = blockX; x < endX; ++x) {
                    const double valueA = luma(rowA + x * 4);
                    const double valueB = luma(rowB + x * 4);
                    sumA += valueA;
                    sumB += valueB;
                    squareA += valueA * valueA;
                    squareB += valueB * valueB;
                    product += valueA * valueB;
                    ++count;
                }
            }
            if (count == 0) {
                continue;
            }
            const double divisor = static_cast<double>(count);
            const double meanA = sumA / divisor;
            const double meanB = sumB / divisor;
            const double varianceA = std::max(0.0, squareA / divisor - meanA * meanA);
            const double varianceB = std::max(0.0, squareB / divisor - meanB * meanB);
            const double covariance = product / divisor - meanA * meanB;
            const double numerator =
                (2.0 * meanA * meanB + kSsimC1) * (2.0 * covariance + kSsimC2);
            const double denominator =
                (meanA * meanA + meanB * meanB + kSsimC1)
                * (varianceA + varianceB + kSsimC2);
            const double blockSsim = denominator > 0.0
                ? std::clamp(numerator / denominator, -1.0, 1.0)
                : 1.0;
            weightedSsim += blockSsim * divisor;
            ssimPixels += count;

            if (makeSsimMap) {
                const QRgb color = ssimColor(blockSsim);
                for (int y = blockY; y < endY; ++y) {
                    auto* output = reinterpret_cast<QRgb*>(
                        result.visualization.scanLine(y));
                    std::fill(output + blockX, output + endX, color);
                }
            }
        }
    }

    const double comparedComponents =
        static_cast<double>(a.width()) * static_cast<double>(a.height()) * 3.0;
    result.metrics.meanSquaredError = squaredError / comparedComponents;
    result.metrics.psnrDb = result.metrics.meanSquaredError == 0.0
        ? std::numeric_limits<double>::infinity()
        : 10.0 * std::log10(
            (255.0 * 255.0) / result.metrics.meanSquaredError);
    result.metrics.ssim = squaredError == 0.0
        ? 1.0
        : (ssimPixels > 0
               ? std::clamp(
                     weightedSsim / static_cast<double>(ssimPixels),
                     0.0,
                     1.0)
               : 1.0);
    result.metrics.dimensions = a.size();
    result.metrics.comparable = true;
    return result;
}

class FrameComparisonManager::Impl final {
public:
    struct Task final {
        quint64 generation = 0;
        QImage frameA;
        QImage frameB;
        ComparisonMode mode = ComparisonMode::SideBySide;
        int amplification = 4;
    };

    explicit Impl(FrameComparisonManager* owner)
        : owner_(owner)
        , worker_([this](const std::stop_token stop) { run(stop); })
    {
    }

    ~Impl()
    {
        {
            std::lock_guard lock(mutex_);
            closing_ = true;
            activeCancellation_.requestCancellation();
            pending_.reset();
        }
        worker_.request_stop();
        condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    [[nodiscard]] quint64 request(
        QImage frameA,
        QImage frameB,
        const ComparisonMode mode,
        const int amplification)
    {
        std::lock_guard lock(mutex_);
        generation_ = generation_ == std::numeric_limits<quint64>::max()
            ? 1
            : generation_ + 1;
        activeCancellation_.requestCancellation();
        pending_ = Task{
            generation_,
            std::move(frameA),
            std::move(frameB),
            mode,
            std::clamp(amplification, 1, 16),
        };
        condition_.notify_all();
        return generation_;
    }

    void cancel() noexcept
    {
        std::lock_guard lock(mutex_);
        generation_ = generation_ == std::numeric_limits<quint64>::max()
            ? 1
            : generation_ + 1;
        activeCancellation_.requestCancellation();
        pending_.reset();
        condition_.notify_all();
    }

    [[nodiscard]] quint64 generation() const noexcept
    {
        std::lock_guard lock(mutex_);
        return generation_;
    }

    [[nodiscard]] bool accepts(const quint64 generation) const noexcept
    {
        std::lock_guard lock(mutex_);
        return !closing_ && generation == generation_;
    }

private:
    void run(const std::stop_token stop)
    {
        while (!stop.stop_requested()) {
            Task task;
            core::CancellationToken cancellation;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, stop, [this] {
                    return closing_ || pending_.has_value();
                });
                if (closing_ || stop.stop_requested()) {
                    return;
                }
                task = std::move(*pending_);
                pending_.reset();
                activeCancellation_ = core::CancellationSource{};
                cancellation = activeCancellation_.token();
            }

            auto result = FrameComparison::analyze(
                task.frameA,
                task.frameB,
                task.mode,
                task.amplification,
                cancellation);
            result.generation = task.generation;
            if (result.cancelled || !accepts(task.generation)) {
                continue;
            }
            QPointer<FrameComparisonManager> guard(owner_);
            QMetaObject::invokeMethod(
                owner_,
                [guard, result = std::move(result)]() mutable {
                    if (guard) {
                        guard->deliver(std::move(result));
                    }
                },
                Qt::QueuedConnection);
        }
    }

    FrameComparisonManager* const owner_;
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::optional<Task> pending_;
    core::CancellationSource activeCancellation_;
    quint64 generation_ = 0;
    bool closing_ = false;
    std::jthread worker_;
};

FrameComparisonManager::FrameComparisonManager(QObject* parent)
    : QObject(parent)
    , impl_(std::make_unique<Impl>(this))
{
    setObjectName(QStringLiteral("frameComparisonManager"));
}

FrameComparisonManager::~FrameComparisonManager() = default;

quint64 FrameComparisonManager::request(
    QImage frameA,
    QImage frameB,
    const ComparisonMode mode,
    const int differenceAmplification)
{
    return impl_->request(
        std::move(frameA),
        std::move(frameB),
        mode,
        differenceAmplification);
}

void FrameComparisonManager::cancel() noexcept
{
    impl_->cancel();
}

quint64 FrameComparisonManager::generation() const noexcept
{
    return impl_->generation();
}

void FrameComparisonManager::deliver(ComparisonResult result)
{
    if (impl_->accepts(result.generation)) {
        emit comparisonReady(result);
    }
}

} // namespace vidscope::inspection
