#pragma once

#include "export/ExportTypes.h"
#include "media/MediaTypes.h"

#include <QtCore/QObject>
#include <QtCore/QString>

#include <memory>

namespace vidscope::exporting {

// GUI-facing adapter for one bounded, thread-confined export decoder. At most
// one request is pending or active; decoded full-resolution images are written
// immediately and never accumulated as a frame sequence in memory.
class ExportManager final : public QObject {
    Q_OBJECT

public:
    explicit ExportManager(QObject* parent = nullptr);
    ~ExportManager() override;
    ExportManager(const ExportManager&) = delete;
    ExportManager& operator=(const ExportManager&) = delete;

    void setMedia(media::MediaInfoPtr info);
    void clearMedia();
    [[nodiscard]] bool startExport(ExportRequest request);
    void cancel();
    [[nodiscard]] bool isBusy() const noexcept;
    [[nodiscard]] ExportState state() const noexcept;

signals:
    void stateChanged(vidscope::exporting::ExportState state);
    void progressChanged(
        quint64 generation,
        quint64 completed,
        quint64 total,
        const QString& detail);
    void exportFinished(const vidscope::exporting::ExportSummary& summary);

private:
    void deliverProgress(
        quint64 generation,
        quint64 completed,
        quint64 total,
        QString detail);
    void deliverState(quint64 generation, ExportState state);
    void deliverFinished(ExportSummary summary);

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vidscope::exporting
