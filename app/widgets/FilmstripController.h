#pragma once

#include "filmstrip/FilmstripModel.h"
#include "media/MediaTypes.h"
#include "thumbnails/ThumbnailManager.h"

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QSize>
#include <QtCore/QTimer>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vidscope::timeline {
class TimelineWidget;
}

namespace vidscope::widgets {

class FilmstripWidget;

struct FilmstripControllerConfig final {
    QSize targetSize{200, 112};
    int rangeRefreshDebounceMilliseconds = 80;
    int playheadRefreshMilliseconds = 500;
    int cancelledRetryMilliseconds = 250;
};

// Coordinates one bounded filmstrip batch at a time. ThumbnailManager owns all
// worker threads; this object remains GUI-thread-only and rejects deliveries
// from superseded batches before touching the widget.
class FilmstripController final : public QObject {
    Q_OBJECT

public:
    FilmstripController(
        timeline::TimelineWidget* timeline,
        thumbnails::ThumbnailManager* manager,
        FilmstripWidget* widget,
        FilmstripControllerConfig config = {},
        QObject* parent = nullptr);
    ~FilmstripController() override;

    void setMedia(media::MediaInfoPtr info);
    void clear();

    void setMode(filmstrip::FilmstripMode mode);
    [[nodiscard]] filmstrip::FilmstripMode mode() const noexcept;

    void setCount(std::size_t count);
    [[nodiscard]] std::size_t count() const noexcept;

    void setPlayhead(qint64 nanoseconds);
    void notifyFrameObserved();
    void refreshNow();

    [[nodiscard]] qsizetype pendingRequestCount() const noexcept;
    [[nodiscard]] quint64 batchGeneration() const noexcept;

private:
    struct PendingDelivery final {
        quint64 batch = 0;
        std::size_t itemIndex = 0;
    };

    void scheduleRangeRefresh();
    void schedulePlayheadRefresh();
    void cancelFilmstripRequests() noexcept;
    void handlePreview(const thumbnails::ThumbnailResult& result);
    void handleFailure(thumbnails::ThumbnailGeneration generation, const QString& detail);
    void handleCancellation(thumbnails::ThumbnailGeneration generation);
    void retryCancelledRequests();
    void submitTarget(std::size_t itemIndex, quint64 batch);
    [[nodiscard]] thumbnails::ThumbnailPriority requestPriority() const noexcept;
    [[nodiscard]] quint64 nextBatchGeneration() noexcept;

    timeline::TimelineWidget* const timeline_;
    thumbnails::ThumbnailManager* const manager_;
    FilmstripWidget* const widget_;
    FilmstripControllerConfig config_;
    filmstrip::FilmstripModel model_;
    media::MediaInfoPtr mediaInfo_;
    QTimer rangeRefreshTimer_;
    QTimer playheadRefreshTimer_;
    QTimer cancelledRetryTimer_;
    QHash<thumbnails::ThumbnailGeneration, PendingDelivery> pending_;
    std::vector<std::size_t> cancelledItems_;
    quint64 batchGeneration_ = 0;
    qint64 playheadNanoseconds_ = 0;
};

} // namespace vidscope::widgets
