#pragma once

#include "media/MediaTypes.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>

namespace vidscope::playback {

class FrameQueue final {
public:
    FrameQueue(std::size_t maxFrames, std::size_t maxBytes)
        : maxFrames_(maxFrames), maxBytes_(maxBytes)
    {
    }

    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    [[nodiscard]] bool tryPush(media::DecodedFramePtr frame)
    {
        if (!frame) {
            return false;
        }
        std::lock_guard lock(mutex_);
        const auto bytes = frame->estimatedBytes();
        if (closed_ || maxFrames_ == 0 || bytes > maxBytes_ || queue_.size() >= maxFrames_
            || bytes_ > maxBytes_ - bytes) {
            return false;
        }
        bytes_ += bytes;
        queue_.push_back(std::move(frame));
        notEmpty_.notify_one();
        return true;
    }

    [[nodiscard]] bool waitPush(media::DecodedFramePtr frame, std::stop_token stop)
    {
        if (!frame) {
            return false;
        }
        const auto bytes = frame->estimatedBytes();
        if (maxFrames_ == 0 || bytes > maxBytes_) {
            return false;
        }
        std::unique_lock lock(mutex_);
        const bool ready = notFull_.wait(lock, stop, [this, bytes] {
            return closed_ || (queue_.size() < maxFrames_ && bytes_ <= maxBytes_ - bytes);
        });
        if (!ready || closed_) {
            return false;
        }
        bytes_ += bytes;
        queue_.push_back(std::move(frame));
        notEmpty_.notify_one();
        return true;
    }

    [[nodiscard]] std::optional<media::DecodedFramePtr> tryPop()
    {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        auto frame = std::move(queue_.front());
        queue_.pop_front();
        bytes_ -= frame->estimatedBytes();
        notFull_.notify_one();
        return frame;
    }

    [[nodiscard]] std::optional<media::DecodedFramePtr> waitPop(std::stop_token stop)
    {
        std::unique_lock lock(mutex_);
        const bool ready = notEmpty_.wait(lock, stop, [this] { return closed_ || !queue_.empty(); });
        if (!ready || queue_.empty()) {
            return std::nullopt;
        }
        auto frame = std::move(queue_.front());
        queue_.pop_front();
        bytes_ -= frame->estimatedBytes();
        notFull_.notify_one();
        return frame;
    }

    void clear()
    {
        std::lock_guard lock(mutex_);
        queue_.clear();
        bytes_ = 0;
        notFull_.notify_all();
    }

    void close()
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        notEmpty_.notify_all();
        notFull_.notify_all();
    }

    [[nodiscard]] std::size_t size() const
    {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]] std::size_t bytes() const
    {
        std::lock_guard lock(mutex_);
        return bytes_;
    }

private:
    const std::size_t maxFrames_;
    const std::size_t maxBytes_;
    mutable std::mutex mutex_;
    std::condition_variable_any notEmpty_;
    std::condition_variable_any notFull_;
    std::deque<media::DecodedFramePtr> queue_;
    std::size_t bytes_ = 0;
    bool closed_ = false;
};

} // namespace vidscope::playback
