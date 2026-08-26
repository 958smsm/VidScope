#include "inspection/FrameHistory.h"

#include <algorithm>

namespace vidscope::inspection {

FrameHistory::FrameHistory(const std::size_t maximumEntries)
    : maximumEntries_(std::max<std::size_t>(1, maximumEntries))
{
    entries_.reserve(maximumEntries_);
}

bool FrameHistory::visit(const media::DecodedFrame& frame)
{
    if (frame.presentationTime == media::kNoMediaTime) {
        return false;
    }
    FrameHistoryEntry entry{
        frame.id,
        frame.presentationTime,
        frame.duration,
        frame.keyFrame,
        frame.pictureType,
    };
    if (currentIndex_ && sameIdentity(entries_[*currentIndex_], entry)) {
        entries_[*currentIndex_] = entry;
        return false;
    }

    if (currentIndex_ && *currentIndex_ + 1 < entries_.size()) {
        entries_.erase(
            entries_.begin() + static_cast<std::ptrdiff_t>(*currentIndex_ + 1),
            entries_.end());
    }
    entries_.push_back(entry);
    if (entries_.size() > maximumEntries_) {
        entries_.erase(entries_.begin());
    }
    currentIndex_ = entries_.size() - 1;
    return true;
}

std::optional<FrameHistoryEntry> FrameHistory::back()
{
    if (!canGoBack()) {
        return std::nullopt;
    }
    --*currentIndex_;
    return entries_[*currentIndex_];
}

std::optional<FrameHistoryEntry> FrameHistory::forward()
{
    if (!canGoForward()) {
        return std::nullopt;
    }
    ++*currentIndex_;
    return entries_[*currentIndex_];
}

void FrameHistory::clear() noexcept
{
    entries_.clear();
    currentIndex_.reset();
}

bool FrameHistory::canGoBack() const noexcept
{
    return currentIndex_ && *currentIndex_ > 0;
}

bool FrameHistory::canGoForward() const noexcept
{
    return currentIndex_ && *currentIndex_ + 1 < entries_.size();
}

std::span<const FrameHistoryEntry> FrameHistory::entries() const noexcept
{
    return entries_;
}

std::optional<std::size_t> FrameHistory::currentIndex() const noexcept
{
    return currentIndex_;
}

std::size_t FrameHistory::maximumEntries() const noexcept
{
    return maximumEntries_;
}

bool FrameHistory::sameIdentity(
    const FrameHistoryEntry& left,
    const FrameHistoryEntry& right) noexcept
{
    if (left.id.sessionSerial != 0 && left.id.sessionSerial == right.id.sessionSerial) {
        return true;
    }
    if (left.id.presentationIndex >= 0 && right.id.presentationIndex >= 0) {
        return left.id.presentationIndex == right.id.presentationIndex;
    }
    return left.time == right.time && left.id.pts == right.id.pts;
}

} // namespace vidscope::inspection
