#pragma once

#include "thread_policy.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include <rt/runtime.hpp>

namespace rt::detail {

// A fixed service lane detects an expired frame budget but never invokes host
// code or mutates degradation state. The calling frame thread completes the
// arm, consumes the one-shot result, and applies degradation.
class WatchdogMonitor final {
public:
    WatchdogMonitor() = default;
    ~WatchdogMonitor();

    WatchdogMonitor(const WatchdogMonitor&) = delete;
    WatchdogMonitor& operator=(const WatchdogMonitor&) = delete;

    [[nodiscard]] Status start(
        ThreadPolicyProvider& provider,
        ThreadStartupGate& gate,
        const ThreadRolePlan& plan) noexcept;
    void stop() noexcept;
    void wait_started() const noexcept;
    [[nodiscard]] const ThreadStartupResult& startup_result() const noexcept {
        return startup_result_;
    }

    [[nodiscard]] std::uint64_t arm(
        std::uint64_t absolute_deadline_ns,
        std::uint64_t timeout_ns) noexcept;
    [[nodiscard]] bool complete(
        std::uint64_t token,
        std::uint64_t finish_ns) noexcept;
    [[nodiscard]] bool has_fired(std::uint64_t token) const noexcept;

private:
    [[nodiscard]] bool try_fire(
        std::uint64_t token,
        std::uint64_t now_ns) noexcept;
    void run() noexcept;
    static void run_entry(void* monitor) noexcept;

    static constexpr std::uint64_t kFiredBit =
        std::uint64_t{1} << 63;
    static constexpr std::uint64_t kTokenMask = ~kFiredBit;

    NativeThread thread_;
    ThreadStartupResult startup_result_{};
    WaitStrategy wait_strategy_ = WaitStrategy::park;
    std::mutex service_mutex_;
    std::condition_variable service_cv_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<std::uint64_t> active_state_{0};
    std::atomic<std::uint64_t> runtime_deadline_ns_{0};
    std::atomic<std::uint64_t> service_deadline_ns_{0};
    std::uint64_t next_token_ = 0;
};

} // namespace rt::detail
