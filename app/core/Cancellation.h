#pragma once

#include <atomic>
#include <memory>

namespace vidscope::core {

class CancellationToken final {
public:
    CancellationToken() = default;

    [[nodiscard]] bool isCancellationRequested() const noexcept
    {
        return flag_ && flag_->load(std::memory_order_acquire);
    }

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return static_cast<bool>(flag_);
    }

private:
    explicit CancellationToken(std::shared_ptr<std::atomic_bool> flag)
        : flag_(std::move(flag))
    {
    }

    std::shared_ptr<std::atomic_bool> flag_;
    friend class CancellationSource;
};

class CancellationSource final {
public:
    CancellationSource()
        : flag_(std::make_shared<std::atomic_bool>(false))
    {
    }

    [[nodiscard]] CancellationToken token() const noexcept
    {
        return CancellationToken(flag_);
    }

    void requestCancellation() noexcept
    {
        flag_->store(true, std::memory_order_release);
    }

    [[nodiscard]] bool isCancellationRequested() const noexcept
    {
        return flag_->load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<std::atomic_bool> flag_;
};

} // namespace vidscope::core
