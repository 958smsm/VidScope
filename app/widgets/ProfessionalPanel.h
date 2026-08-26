#pragma once

#include "inspection/FrameHistory.h"
#include "playback/PlaybackController.h"
#include "timeline/TimelineModel.h"

#include <QtWidgets/QWidget>

#include <optional>
#include <span>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace vidscope::widgets {

class ProfessionalPanel final : public QWidget {
    Q_OBJECT

public:
    explicit ProfessionalPanel(QWidget* parent = nullptr);

    void setHistory(
        std::span<const inspection::FrameHistoryEntry> entries,
        std::optional<std::size_t> currentIndex);
    void setMarkers(std::span<const timeline::TimelineMarker> markers);
    void setDiagnostics(const playback::PlaybackDiagnostics& diagnostics);
    void clear();

signals:
    void seekRequested(qint64 timestampNanoseconds);
    void editMarkerRequested(quint64 id);
    void deleteMarkerRequested(quint64 id);

private:
    void activateTimestampItem(QTreeWidgetItem* item);
    [[nodiscard]] std::optional<quint64> selectedMarkerId() const;

    QTabWidget* tabs_ = nullptr;
    QTreeWidget* history_ = nullptr;
    QTreeWidget* markers_ = nullptr;
    QPushButton* editMarker_ = nullptr;
    QPushButton* deleteMarker_ = nullptr;
    QPlainTextEdit* diagnostics_ = nullptr;
};

} // namespace vidscope::widgets
