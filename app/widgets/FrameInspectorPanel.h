#pragma once

#include "analysis/AnalysisTypes.h"
#include "inspection/FrameComparison.h"
#include "media/MediaTypes.h"

#include <QtCore/QHash>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtWidgets/QWidget>

#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace vidscope::widgets {

class PixelMagnifierWidget;

class FrameInspectorPanel final : public QWidget {
    Q_OBJECT

public:
    explicit FrameInspectorPanel(QWidget* parent = nullptr);
    ~FrameInspectorPanel() override;

    void setFrame(
        media::DecodedFramePtr frame,
        QImage image,
        std::optional<analysis::AnalysisSample> analysis = std::nullopt);
    void setAnalysis(std::optional<analysis::AnalysisSample> analysis);
    void setPaused(bool paused);
    void clear();
    void updatePixel(int x, int y, const QColor& rgb);
    void clearPixel();

    [[nodiscard]] bool hasCurrentFrame() const noexcept;
    [[nodiscard]] bool hasFrameA() const noexcept;
    [[nodiscard]] bool hasFrameB() const noexcept;
    [[nodiscard]] inspection::ComparisonMode comparisonMode() const noexcept;
    [[nodiscard]] const inspection::ComparisonMetrics& comparisonMetrics() const noexcept;

public slots:
    void setCurrentAsFrameA();
    void setCurrentAsFrameB();
    void clearComparison();

signals:
    void previousFrameRequested();
    void nextFrameRequested();
    void pixelInspectionChanged(bool enabled);
    void imageZoomChanged(double factor);
    void comparisonDisplayChanged(
        const QImage& frameA,
        const QImage& frameB,
        vidscope::inspection::ComparisonMode mode,
        const QImage& visualization,
        const QString& detail);
    void comparisonCleared();

private:
    void addMetadataRow(const QString& key, const QString& label);
    void setMetadata(const QString& key, const QString& value);
    void updateMetadata();
    void updateCaptureLabels();
    void updatePixelAvailability();
    void requestComparison();
    [[nodiscard]] QString frameIdentity(const media::DecodedFramePtr& frame) const;

    media::DecodedFramePtr currentFrame_;
    media::DecodedFramePtr frameA_;
    media::DecodedFramePtr frameB_;
    QImage currentImage_;
    QImage imageA_;
    QImage imageB_;
    std::optional<analysis::AnalysisSample> analysis_;
    inspection::ComparisonMetrics comparisonMetrics_;
    inspection::FrameComparisonManager* comparisonManager_ = nullptr;
    QHash<QString, QTreeWidgetItem*> metadataRows_;
    QTreeWidget* metadata_ = nullptr;
    QPushButton* previousFrame_ = nullptr;
    QPushButton* nextFrame_ = nullptr;
    QComboBox* imageZoom_ = nullptr;
    QCheckBox* pixelEnabled_ = nullptr;
    QComboBox* pixelMagnification_ = nullptr;
    PixelMagnifierWidget* magnifier_ = nullptr;
    QLabel* pixelReadout_ = nullptr;
    QPushButton* setFrameA_ = nullptr;
    QPushButton* setFrameB_ = nullptr;
    QPushButton* clearComparison_ = nullptr;
    QLabel* frameALabel_ = nullptr;
    QLabel* frameBLabel_ = nullptr;
    QComboBox* comparisonMode_ = nullptr;
    QLabel* comparisonMetricsLabel_ = nullptr;
    bool paused_ = false;
};

} // namespace vidscope::widgets

