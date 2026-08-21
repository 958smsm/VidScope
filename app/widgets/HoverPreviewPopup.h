#pragma once

#include "thumbnails/ThumbnailTypes.h"

#include <QtCore/QPoint>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <optional>

class QLabel;

namespace vidscope::widgets {

class HoverPreviewPopup final : public QWidget {
    Q_OBJECT

public:
    explicit HoverPreviewPopup(QWidget* anchorWindow);

    void setPreviewSize(QSize size);

    void showPending(
        thumbnails::ThumbnailGeneration generation,
        qint64 timestampNanoseconds,
        qint64 presentationIndexHint,
        QPoint globalCursorPosition);
    void showPreview(
        const thumbnails::ThumbnailResult& result,
        QPoint globalCursorPosition);
    void showFailure(
        thumbnails::ThumbnailGeneration generation,
        qint64 timestampNanoseconds,
        qint64 presentationIndexHint,
        const QString& detail,
        QPoint globalCursorPosition);
    void updateCursorPosition(QPoint globalCursorPosition);
    void dismiss();

    [[nodiscard]] thumbnails::ThumbnailGeneration generation() const noexcept;
    [[nodiscard]] qint64 displayedTimestampNanoseconds() const noexcept;
    [[nodiscard]] bool hasPreviewImage() const noexcept;

private:
    void positionNear(QPoint globalCursorPosition);
    void updateMetadata(
        qint64 timestampNanoseconds,
        qint64 presentationIndex,
        qint64 presentationIndexHint,
        AVPictureType pictureType,
        bool keyFrame,
        std::optional<float> motionScore,
        std::optional<float> similarityScore,
        thumbnails::ThumbnailCacheSource cacheSource);

    QLabel* imageLabel_ = nullptr;
    QLabel* frameLabel_ = nullptr;
    QLabel* timeLabel_ = nullptr;
    QLabel* detailLabel_ = nullptr;
    QLabel* analysisLabel_ = nullptr;
    thumbnails::ThumbnailGeneration generation_ = 0;
    qint64 displayedTimestampNanoseconds_ = 0;
    bool hasPreviewImage_ = false;
};

} // namespace vidscope::widgets
