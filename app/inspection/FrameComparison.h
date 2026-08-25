#pragma once

#include "core/Cancellation.h"

#include <QtCore/QObject>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QtTypes>
#include <QtGui/QImage>

#include <cstdint>
#include <memory>

namespace vidscope::inspection {

enum class ComparisonMode : std::uint8_t {
    SideBySide,
    Overlay,
    Wipe,
    Blink,
    AbsoluteDifference,
    AmplifiedDifference,
    SsimMap,
};

struct ComparisonMetrics final {
    bool comparable = false;
    double ssim = 0.0;
    double psnrDb = 0.0;
    double meanSquaredError = 0.0;
    QSize dimensions;
    QString detail;

    friend bool operator==(const ComparisonMetrics&, const ComparisonMetrics&) = default;
};

struct ComparisonResult final {
    quint64 generation = 0;
    ComparisonMode mode = ComparisonMode::SideBySide;
    ComparisonMetrics metrics;
    QImage visualization;
    bool cancelled = false;
};

class FrameComparison final {
public:
    [[nodiscard]] static ComparisonResult analyze(
        const QImage& frameA,
        const QImage& frameB,
        ComparisonMode mode,
        int differenceAmplification = 4,
        core::CancellationToken cancellation = {});
};

// Owns one coalescing worker. Requests retain at most two implicitly-shared
// source images and only the newest request is eligible for GUI delivery.
class FrameComparisonManager final : public QObject {
    Q_OBJECT

public:
    explicit FrameComparisonManager(QObject* parent = nullptr);
    ~FrameComparisonManager() override;
    FrameComparisonManager(const FrameComparisonManager&) = delete;
    FrameComparisonManager& operator=(const FrameComparisonManager&) = delete;

    [[nodiscard]] quint64 request(
        QImage frameA,
        QImage frameB,
        ComparisonMode mode,
        int differenceAmplification = 4);
    void cancel() noexcept;
    [[nodiscard]] quint64 generation() const noexcept;

signals:
    void comparisonReady(const vidscope::inspection::ComparisonResult& result);

private:
    void deliver(ComparisonResult result);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vidscope::inspection

