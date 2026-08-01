#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include <rt/runtime.hpp>

namespace rt::detail {

class ThreadPolicyTransaction final {
public:
    ThreadPolicyTransaction() = default;
    ThreadPolicyTransaction(const ThreadPolicyTransaction&) = delete;
    ThreadPolicyTransaction& operator=(const ThreadPolicyTransaction&) = delete;

    void begin(
        ThreadPolicyProvider& provider,
        std::span<ThreadPolicyReport> reports) noexcept;
    [[nodiscard]] Status verify_frame_thread() noexcept;
    void prepare_current_thread(ThreadResourceId id) noexcept;
    [[nodiscard]] bool await_decision(ThreadResourceId id) noexcept;
    void commit() noexcept;
    void abort() noexcept;
    [[nodiscard]] Status failure() const noexcept;
    [[nodiscard]] WaitStrategy wait_strategy(
        ThreadResourceId id) const noexcept;

private:
    enum class Decision : std::uint8_t {
        pending,
        commit,
        abort,
    };

    [[nodiscard]] ThreadPolicyReport* find(ThreadResourceId id) noexcept;
    [[nodiscard]] const ThreadPolicyReport* find(
        ThreadResourceId id) const noexcept;
    void record_failure(Status status) noexcept;
    void wait_for_released() noexcept;

    ThreadPolicyProvider* provider_ = nullptr;
    std::span<ThreadPolicyReport> reports_{};
    std::atomic<Decision> decision_{Decision::pending};
    std::atomic<std::size_t> arrived_{0};
    std::atomic<std::size_t> released_{0};
    std::atomic<Status> failure_{Status::ok};
};

} // namespace rt::detail
