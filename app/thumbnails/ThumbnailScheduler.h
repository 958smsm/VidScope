#pragma once

#include "core/Cancellation.h"
#include "thumbnails/ThumbnailCache.h"
#include "thumbnails/ThumbnailTypes.h"

#include <QtCore/QString>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>

namespace vidscope::thumbnails {

struct ThumbnailMediaSource final {
    std::filesystem::path path;
    QString identity;
    std::uint64_t epoch = 0;
};

struct ThumbnailJob final {
    ThumbnailRequest request;
    ThumbnailCacheKey cacheKey;
    ThumbnailMediaSource media;
    std::shared_ptr<core::CancellationSource> cancellation;
    std::uint64_t sequence = 0;
};

enum class ThumbnailTakeStatus : std::uint8_t {
    Job,
    Maintenance,
    Closed,
};

struct ThumbnailTakeResult final {
    ThumbnailTakeStatus status = ThumbnailTakeStatus::Closed;
    std::optional<ThumbnailJob> job;
    std::uint64_t maintenanceGeneration = 0;
};

class ThumbnailScheduler final {
public:
    explicit ThumbnailScheduler(std::size_t maximumPending, std::size_t workerSlots);
    ~ThumbnailScheduler();
    ThumbnailScheduler(const ThumbnailScheduler&) = delete;
    ThumbnailScheduler& operator=(const ThumbnailScheduler&) = delete;

    bool schedule(ThumbnailJob job);
    [[nodiscard]] ThumbnailTakeResult waitTake(
        std::size_t workerSlot,
        std::stop_token stop,
        std::uint64_t observedMaintenanceGeneration);
    void complete(std::size_t workerSlot, ThumbnailGeneration generation) noexcept;

    [[nodiscard]] std::uint64_t requestMaintenance();
    void supersede(ThumbnailPriority priority) noexcept;
    void cancelPriority(ThumbnailPriority priority) noexcept;
    void cancelAll() noexcept;
    void close() noexcept;

    [[nodiscard]] std::size_t pendingCount() const noexcept;
    [[nodiscard]] bool isClosed() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vidscope::thumbnails
