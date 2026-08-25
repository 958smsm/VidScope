#pragma once

#include "filmstrip/FilmstripModel.h"
#include "thumbnails/ThumbnailTypes.h"

#include <QtCore/QRectF>
#include <QtCore/QSize>
#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vidscope::widgets {

enum class FilmstripItemState : std::uint8_t {
    Loading,
    Ready,
    Failed,
};

struct FilmstripItem final {
    filmstrip::FilmstripTarget target;
    FilmstripItemState state = FilmstripItemState::Loading;
    std::optional<thumbnails::ThumbnailFrame> frame;
    QString failureDetail;
};

// A single custom-painted surface. It intentionally does not create one child
// QWidget per thumbnail, so 32/custom-count strips stay lightweight.
class FilmstripWidget final : public QWidget {
    Q_OBJECT

public:
    explicit FilmstripWidget(QWidget* parent = nullptr);

    void setPlan(filmstrip::FilmstripPlan plan);
    void clear(QString message = {});
    bool setThumbnail(std::size_t index, thumbnails::ThumbnailFrame frame);
    bool setAnalysis(
        std::size_t index,
        std::optional<float> motion,
        std::optional<float> similarity);
    bool setFailure(std::size_t index, QString detail);
    void setPlayhead(qint64 nanoseconds);

    [[nodiscard]] const filmstrip::FilmstripPlan& plan() const noexcept;
    [[nodiscard]] std::size_t itemCount() const noexcept;
    [[nodiscard]] std::size_t readyItemCount() const noexcept;
    [[nodiscard]] std::size_t failedItemCount() const noexcept;
    [[nodiscard]] const FilmstripItem* item(std::size_t index) const noexcept;
    [[nodiscard]] QRectF itemRect(std::size_t index) const noexcept;
    [[nodiscard]] QSize preferredThumbnailSize() const noexcept;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

signals:
    void seekRequested(qint64 nanoseconds);
    void frameInspectorRequested(qint64 nanoseconds, qint64 presentationIndex);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    [[nodiscard]] std::optional<std::size_t> indexAt(QPointF position) const noexcept;
    [[nodiscard]] qint64 itemTimestamp(const FilmstripItem& item) const noexcept;
    [[nodiscard]] qint64 itemPresentationIndex(const FilmstripItem& item) const noexcept;
    void activateItem(std::size_t index);
    void inspectItem(std::size_t index);
    void updateItemToolTip(std::optional<std::size_t> index);

    filmstrip::FilmstripPlan plan_;
    std::vector<FilmstripItem> items_;
    QString emptyMessage_;
    qint64 playheadNanoseconds_ = 0;
    std::optional<std::size_t> pressedIndex_;
    std::optional<std::size_t> selectedIndex_;
    std::optional<std::size_t> hoveredIndex_;
};

} // namespace vidscope::widgets
