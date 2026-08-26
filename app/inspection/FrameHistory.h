#pragma once

#include "media/MediaTypes.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace vidscope::inspection {

struct FrameHistoryEntry final {
    media::FrameId id;
    media::MediaTime time{};
    media::MediaTime duration{};
    bool keyFrame = false;
    AVPictureType pictureType = AV_PICTURE_TYPE_NONE;

    friend bool operator==(const FrameHistoryEntry&, const FrameHistoryEntry&) = default;
};

// GUI-thread-owned browser-style inspection history. Visiting a frame after
// navigating backward discards the stale forward branch. Storage is bounded.
class FrameHistory final {
public:
    static constexpr std::size_t kDefaultMaximumEntries = 128;

    explicit FrameHistory(
        std::size_t maximumEntries = kDefaultMaximumEntries);

    [[nodiscard]] bool visit(const media::DecodedFrame& frame);
    [[nodiscard]] std::optional<FrameHistoryEntry> back();
    [[nodiscard]] std::optional<FrameHistoryEntry> forward();
    void clear() noexcept;

    [[nodiscard]] bool canGoBack() const noexcept;
    [[nodiscard]] bool canGoForward() const noexcept;
    [[nodiscard]] std::span<const FrameHistoryEntry> entries() const noexcept;
    [[nodiscard]] std::optional<std::size_t> currentIndex() const noexcept;
    [[nodiscard]] std::size_t maximumEntries() const noexcept;

private:
    [[nodiscard]] static bool sameIdentity(
        const FrameHistoryEntry& left,
        const FrameHistoryEntry& right) noexcept;

    std::vector<FrameHistoryEntry> entries_;
    std::size_t maximumEntries_ = 0;
    std::optional<std::size_t> currentIndex_;
};

} // namespace vidscope::inspection
