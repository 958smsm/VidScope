#pragma once

#include "analysis/AnalysisCache.h"
#include "analysis/AnalysisPyramid.h"
#include "analysis/AnalysisTypes.h"
#include "analysis/DetectionEngine.h"
#include "media/MediaTypes.h"
#include "playback/PlaybackSession.h"

#include <QtCore/QObject>
#include <QtCore/QSize>
#include <QtCore/QString>

#include <cstddef>
#include <memory>
#include <optional>

namespace vidscope::analysis {

struct AnalysisManagerConfig final {
    QSize lumaSize{160, 90};
    std::size_t maximumInMemorySamples = 2'000'000;
    media::MediaTime playheadRadius = std::chrono::seconds(2);
    media::MediaTime rangePreroll = std::chrono::seconds(1);
    std::size_t deliveryBatchFrames = 32;
    AnalysisPyramidConfig pyramid;
    DetectionConfig detection;
    playback::PlaybackSessionConfig session;
    AnalysisCacheConfig cache;
};

// GUI-facing coordinator for one thread-confined decoder/analyzer worker.
// Requests are timestamp-authoritative, coalesced by priority, cancellable, and
// paused while playback is active so background analysis cannot compete with it.
class AnalysisManager final : public QObject {
    Q_OBJECT

public:
    explicit AnalysisManager(AnalysisManagerConfig config = {}, QObject* parent = nullptr);
    ~AnalysisManager() override;
    AnalysisManager(const AnalysisManager&) = delete;
    AnalysisManager& operator=(const AnalysisManager&) = delete;

    void setMedia(media::MediaInfoPtr info);
    void clearMedia();
    void setPlaybackActive(bool active);
    void setInteractiveActivity(bool active);
    void requestPlayhead(qint64 timestampNanoseconds);
    void requestVisibleRange(qint64 startNanoseconds, qint64 endNanoseconds);
    void setDetectionConfig(DetectionConfig config);
    void reanalyzeDetections();

    [[nodiscard]] std::optional<AnalysisSample> sampleFor(
        qint64 timestampNanoseconds,
        qint64 presentationIndex = -1) const;
    [[nodiscard]] std::vector<AnalysisSample> samplesInRange(
        qint64 startNanoseconds,
        qint64 endNanoseconds,
        std::size_t maximumResults = 100'000) const;
    [[nodiscard]] AnalysisLodView lodView(
        qint64 startNanoseconds,
        qint64 endNanoseconds,
        std::size_t maximumBuckets) const;
    [[nodiscard]] DetectionConfig detectionConfig() const;
    [[nodiscard]] DetectionResults detectionResults() const;
    [[nodiscard]] qsizetype sampleCount() const noexcept;
    [[nodiscard]] double progress() const noexcept;
    [[nodiscard]] AnalysisState state() const noexcept;

signals:
    void samplesAvailable(qint64 startNanoseconds, qint64 endNanoseconds, quint64 totalSamples);
    void progressChanged(double progress, quint64 analyzedSamples);
    void stateChanged(vidscope::analysis::AnalysisState state);
    void detectionsChanged(
        quint64 sceneCount,
        quint64 duplicateCount,
        quint64 freezeCount,
        quint64 analyzedSamples);
    void errorOccurred(const QString& detail);

private:
    void deliverSamples(qint64 start, qint64 end, quint64 count, quint64 epoch);
    void deliverProgress(double progress, quint64 count, quint64 epoch);
    void deliverState(AnalysisState state, quint64 epoch);
    void deliverDetections(
        quint64 sceneCount,
        quint64 duplicateCount,
        quint64 freezeCount,
        quint64 analyzedSamples,
        quint64 epoch);
    void deliverError(QString detail, quint64 epoch);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vidscope::analysis

Q_DECLARE_METATYPE(vidscope::analysis::AnalysisState)
