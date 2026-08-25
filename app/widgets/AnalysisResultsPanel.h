#pragma once

#include "analysis/DetectionEngine.h"
#include "thumbnails/ThumbnailTypes.h"

#include <QtCore/QHash>
#include <QtCore/QPointer>
#include <QtWidgets/QWidget>

class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace vidscope::thumbnails {
class ThumbnailManager;
}

namespace vidscope::widgets {

class AnalysisResultsPanel final : public QWidget {
    Q_OBJECT

public:
    explicit AnalysisResultsPanel(QWidget* parent = nullptr);

    void setThumbnailManager(thumbnails::ThumbnailManager* manager);
    void setDetectionConfig(const analysis::DetectionConfig& config);
    void setResults(const analysis::DetectionResults& results);
    void clearResults();

signals:
    void seekRequested(qint64 timestampNanoseconds);
    void reanalyzeRequested(vidscope::analysis::DetectionConfig config);

private:
    [[nodiscard]] analysis::DetectionConfig configFromControls() const;
    void activateItem(QTreeWidgetItem* item);
    void requestSceneThumbnails();
    void handleThumbnail(const thumbnails::ThumbnailResult& result);
    void configureTree(QTreeWidget* tree);

    QPointer<thumbnails::ThumbnailManager> thumbnailManager_;
    analysis::DetectionConfig config_;
    analysis::DetectionResults results_;
    QHash<thumbnails::ThumbnailGeneration, QTreeWidgetItem*> pendingThumbnails_;
    QLabel* summary_ = nullptr;
    QDoubleSpinBox* sceneThreshold_ = nullptr;
    QDoubleSpinBox* duplicateThreshold_ = nullptr;
    QDoubleSpinBox* freezeThreshold_ = nullptr;
    QSpinBox* freezeDuration_ = nullptr;
    QPushButton* reanalyze_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QTreeWidget* scenes_ = nullptr;
    QTreeWidget* duplicates_ = nullptr;
    QTreeWidget* freezes_ = nullptr;
};

} // namespace vidscope::widgets
