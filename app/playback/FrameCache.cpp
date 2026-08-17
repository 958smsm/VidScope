#include "playback/FrameCache.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace vidscope::playback {
namespace {

[[nodiscard]] bool presentationBefore(
    const media::DecodedFrame& left,
    const media::DecodedFrame& right) noexcept
{
    if (left.presentationTime != right.presentationTime) {
        return left.presentationTime < right.presentationTime;
    }
    return left.id.sessionSerial < right.id.sessionSerial;
}

[[nodiscard]] bool isImmediateNeighbor(
    const media::DecodedFrame& current,
    const media::DecodedFrame& candidate,
    const bool forward) noexcept
{
    const bool currentHasIndex = current.id.presentationIndex >= 0;
    const bool candidateHasIndex = candidate.id.presentationIndex >= 0;
    if (currentHasIndex != candidateHasIndex) {
        return false;
    }

    if (currentHasIndex) {
        if (forward) {
            return current.id.presentationIndex != std::numeric_limits<std::int64_t>::max()
                && candidate.id.presentationIndex == current.id.presentationIndex + 1;
        }
        return current.id.presentationIndex != std::numeric_limits<std::int64_t>::min()
            && candidate.id.presentationIndex == current.id.presentationIndex - 1;
    }

    if (forward) {
        return current.id.sessionSerial != std::numeric_limits<std::uint64_t>::max()
            && candidate.id.sessionSerial == current.id.sessionSerial + 1;
    }
    return current.id.sessionSerial != 0
        && candidate.id.sessionSerial == current.id.sessionSerial - 1;
}

} // namespace

class FrameCache::Impl final {
public:
    explicit Impl(const std::size_t byteBudget)
        : byteBudget_(byteBudget)
    {
    }

    bool insert(media::DecodedFramePtr frame)
    {
        if (!frame) {
            return false;
        }

        const auto frameBytes = frame->estimatedBytes();
        if (frameBytes == 0) {
            return false;
        }

        std::lock_guard lock(mutex_);
        const auto serial = frame->id.sessionSerial;
        if (const auto existing = entries_.find(serial); existing != entries_.end()) {
            touch(existing->second);
            return true;
        }

        if (frameBytes > byteBudget_ || !canMakeRoom(frameBytes)) {
            return false;
        }
        makeRoom(frameBytes, false);

        Entry entry;
        entry.frame = std::move(frame);
        entry.bytes = frameBytes;
        touch(entry);
        bytes_ += frameBytes;
        entries_.emplace(serial, std::move(entry));
        return true;
    }

    media::DecodedFramePtr find(const std::uint64_t serial)
    {
        std::lock_guard lock(mutex_);
        const auto found = entries_.find(serial);
        if (found == entries_.end()) {
            ++misses_;
            return {};
        }
        ++hits_;
        touch(found->second);
        return found->second.frame;
    }

    media::DecodedFramePtr adjacent(const media::DecodedFrame& current, const bool forward)
    {
        std::lock_guard lock(mutex_);
        Entry* selected = nullptr;
        for (auto& [serial, entry] : entries_) {
            (void)serial;
            if (!isImmediateNeighbor(current, *entry.frame, forward)) {
                continue;
            }
            const bool candidate = forward ? presentationBefore(current, *entry.frame)
                                           : presentationBefore(*entry.frame, current);
            if (!candidate) {
                continue;
            }
            if (selected == nullptr
                || (forward ? presentationBefore(*entry.frame, *selected->frame)
                            : presentationBefore(*selected->frame, *entry.frame))) {
                selected = &entry;
            }
        }
        return finishLookup(selected);
    }

    media::DecodedFramePtr previousKeyframe(const media::DecodedFrame& current)
    {
        std::lock_guard lock(mutex_);
        Entry* selected = nullptr;
        for (auto& [serial, entry] : entries_) {
            (void)serial;
            if (!entry.frame->keyFrame || !presentationBefore(*entry.frame, current)) {
                continue;
            }
            if (selected == nullptr || presentationBefore(*selected->frame, *entry.frame)) {
                selected = &entry;
            }
        }
        return finishLookup(selected);
    }

    std::vector<media::DecodedFramePtr> framesAfter(
        const media::DecodedFrame& current,
        const std::size_t maximumCount)
    {
        std::lock_guard lock(mutex_);
        std::vector<Entry*> ordered;
        if (maximumCount != 0) {
            ordered.reserve(std::min(maximumCount, entries_.size()));
        }
        for (auto& [serial, entry] : entries_) {
            (void)serial;
            if (presentationBefore(current, *entry.frame)) {
                ordered.push_back(&entry);
            }
        }
        std::sort(ordered.begin(), ordered.end(), [](const Entry* left, const Entry* right) {
            return presentationBefore(*left->frame, *right->frame);
        });
        if (ordered.size() > maximumCount) {
            ordered.resize(maximumCount);
        }

        std::vector<media::DecodedFramePtr> result;
        result.reserve(ordered.size());
        for (Entry* entry : ordered) {
            touch(*entry);
            result.push_back(entry->frame);
        }
        if (result.empty()) {
            ++misses_;
        } else {
            ++hits_;
        }
        return result;
    }

    void pin(const std::uint64_t serial)
    {
        std::lock_guard lock(mutex_);
        pinnedSerial_ = serial;
        if (const auto found = entries_.find(serial); found != entries_.end()) {
            touch(found->second);
        }
    }

    void clear()
    {
        std::lock_guard lock(mutex_);
        entries_.clear();
        bytes_ = 0;
        pinnedSerial_.reset();
    }

    void setByteBudget(const std::size_t bytes)
    {
        std::lock_guard lock(mutex_);
        byteBudget_ = bytes;
        makeRoom(0, false);
        if (bytes_ > byteBudget_) {
            // A pin is an eviction preference, not permission to exceed the hard limit.
            pinnedSerial_.reset();
            makeRoom(0, true);
        }
    }

    FrameCacheStats stats() const
    {
        std::lock_guard lock(mutex_);
        return {
            entries_.size(),
            bytes_,
            byteBudget_,
            hits_,
            misses_,
            evictions_,
        };
    }

private:
    struct Entry final {
        media::DecodedFramePtr frame;
        std::size_t bytes = 0;
        std::uint64_t lastUse = 0;
    };

    void touch(Entry& entry) noexcept
    {
        if (accessClock_ == std::numeric_limits<std::uint64_t>::max()) {
            std::uint64_t next = 1;
            std::vector<Entry*> ordered;
            ordered.reserve(entries_.size());
            for (auto& [serial, candidate] : entries_) {
                (void)serial;
                ordered.push_back(&candidate);
            }
            std::sort(ordered.begin(), ordered.end(), [](const Entry* left, const Entry* right) {
                return left->lastUse < right->lastUse;
            });
            for (Entry* candidate : ordered) {
                candidate->lastUse = next++;
            }
            accessClock_ = next;
        }
        entry.lastUse = ++accessClock_;
    }

    media::DecodedFramePtr finishLookup(Entry* entry)
    {
        if (entry == nullptr) {
            ++misses_;
            return {};
        }
        ++hits_;
        touch(*entry);
        return entry->frame;
    }

    [[nodiscard]] bool canMakeRoom(const std::size_t additional) const noexcept
    {
        if (additional > byteBudget_) {
            return false;
        }
        const auto retained = pinnedSerial_.has_value()
            ? [&]() {
                  const auto pinned = entries_.find(*pinnedSerial_);
                  return pinned != entries_.end() ? pinned->second.bytes : std::size_t{0};
              }()
            : std::size_t{0};
        return retained <= byteBudget_ - additional;
    }

    void makeRoom(const std::size_t additional, const bool allowPinned)
    {
        while (bytes_ > byteBudget_ - std::min(additional, byteBudget_)) {
            auto victim = entries_.end();
            for (auto candidate = entries_.begin(); candidate != entries_.end(); ++candidate) {
                if (!allowPinned && pinnedSerial_ && candidate->first == *pinnedSerial_) {
                    continue;
                }
                if (victim == entries_.end()
                    || candidate->second.lastUse < victim->second.lastUse) {
                    victim = candidate;
                }
            }
            if (victim == entries_.end()) {
                return;
            }
            bytes_ -= victim->second.bytes;
            entries_.erase(victim);
            ++evictions_;
        }
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, Entry> entries_;
    std::optional<std::uint64_t> pinnedSerial_;
    std::size_t bytes_ = 0;
    std::size_t byteBudget_ = 0;
    std::uint64_t accessClock_ = 0;
    std::uint64_t hits_ = 0;
    std::uint64_t misses_ = 0;
    std::uint64_t evictions_ = 0;
};

FrameCache::FrameCache(const std::size_t byteBudget)
    : impl_(std::make_unique<Impl>(byteBudget))
{
}

FrameCache::~FrameCache() = default;

bool FrameCache::insert(media::DecodedFramePtr frame)
{
    return impl_->insert(std::move(frame));
}

media::DecodedFramePtr FrameCache::find(const std::uint64_t sessionSerial)
{
    return impl_->find(sessionSerial);
}

media::DecodedFramePtr FrameCache::previous(const media::DecodedFrame& current)
{
    return impl_->adjacent(current, false);
}

media::DecodedFramePtr FrameCache::next(const media::DecodedFrame& current)
{
    return impl_->adjacent(current, true);
}

media::DecodedFramePtr FrameCache::previousKeyframe(const media::DecodedFrame& current)
{
    return impl_->previousKeyframe(current);
}

std::vector<media::DecodedFramePtr> FrameCache::framesAfter(
    const media::DecodedFrame& current,
    const std::size_t maximumCount)
{
    return impl_->framesAfter(current, maximumCount);
}

void FrameCache::pin(const std::uint64_t sessionSerial)
{
    impl_->pin(sessionSerial);
}

void FrameCache::clear()
{
    impl_->clear();
}

void FrameCache::setByteBudget(const std::size_t bytes)
{
    impl_->setByteBudget(bytes);
}

FrameCacheStats FrameCache::stats() const
{
    return impl_->stats();
}

} // namespace vidscope::playback
