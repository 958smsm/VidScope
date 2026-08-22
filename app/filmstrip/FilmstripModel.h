#pragma once

#include "timeline/TimelineModel.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace vidscope::filmstrip {

enum class FilmstripMode : std::uint8_t {
    EntireVideo,
    AroundCurrentPosition,
    VisibleTimeline,
    SelectedRange,
};

enum class FilmstripPlanStatus : std::uint8_t {
    Ready,
    NoMedia,
    SelectionRequired,
};

struct FilmstripTarget final {
    media::MediaTime requestedTime{};
    std::int64_t presentationIndexHint = -1;
    std::optional<bool> keyFrameHint;
    bool current = false;

    friend bool operator==(const FilmstripTarget&, const FilmstripTarget&) = default;
};

struct FilmstripPlan final {
    FilmstripPlanStatus status = FilmstripPlanStatus::NoMedia;
    FilmstripMode mode = FilmstripMode::EntireVideo;
    std::size_t requestedCount = 0;
    media::MediaTime rangeStart{};
    media::MediaTime rangeEnd{};
    bool usesExactContiguousFrames = false;
    std::vector<FilmstripTarget> targets;
};

// GUI-thread-owned policy model. It computes timestamp targets only; decoding,
// cancellation, caching, and delivery remain in the thumbnail subsystem.
class FilmstripModel final {
public:
    static constexpr std::size_t kMinimumCount = 1;
    static constexpr std::size_t kDefaultCount = 16;
    static constexpr std::size_t kMaximumCount = 64;

    void setMode(FilmstripMode mode) noexcept;
    [[nodiscard]] FilmstripMode mode() const noexcept;

    void setCount(std::size_t count) noexcept;
    [[nodiscard]] std::size_t count() const noexcept;

    [[nodiscard]] FilmstripPlan makePlan(const timeline::TimelineModel& timeline) const;

private:
    FilmstripMode mode_ = FilmstripMode::EntireVideo;
    std::size_t count_ = kDefaultCount;
};

} // namespace vidscope::filmstrip
