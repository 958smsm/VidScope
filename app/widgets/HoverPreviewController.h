#pragma once

#include "thumbnails/ThumbnailManager.h"

#include <QtCore/QObject>
#include <QtCore/QPoint>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtCore/QTimer>

class QWidget;

namespace vidscope::timeline {
class TimelineWidget;
}

namespace vidscope::widgets {

class HoverPreviewPopup;

struct HoverPreviewConfig final {
    QSize targetSize{320, 180};
    int debounceMilliseconds = 35;
};

class HoverPreviewController final : public QObject {
    Q_OBJECT

public:
    HoverPreviewController(
        timeline::TimelineWidget* timeline,
        thumbnails::ThumbnailManager* manager,
        QWidget* anchorWindow,
        HoverPreviewConfig config = {},
        QObject* parent = nullptr);
    ~HoverPreviewController() override;

    void clear();
    [[nodiscard]] HoverPreviewPopup* popup() const noexcept;

private:
    void handleHover(
        qint64 timestampNanoseconds,
        qint64 presentationIndex,
        QPoint globalPosition,
        bool active);
    void dispatchRequest();
    void handlePreview(const thumbnails::ThumbnailResult& result);
    void handleFailure(thumbnails::ThumbnailGeneration generation, const QString& detail);

    timeline::TimelineWidget* const timeline_;
    thumbnails::ThumbnailManager* const manager_;
    HoverPreviewPopup* const popup_;
    HoverPreviewConfig config_;
    QTimer debounceTimer_;
    thumbnails::ThumbnailGeneration currentGeneration_ = 0;
    qint64 pendingTimestampNanoseconds_ = 0;
    qint64 pendingPresentationIndex_ = -1;
    QPoint pendingGlobalPosition_;
    bool hoverActive_ = false;
};

} // namespace vidscope::widgets
