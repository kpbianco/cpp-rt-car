#pragma once

#include "owned_thread.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include <rt/runtime.hpp>

namespace rt::detail {

class ThreadPolicyTransaction;

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
        ThreadPolicyTransaction* transaction = nullptr) noexcept;
    [[nodiscard]] Status start(
        MemoryRegionProvider& memory_provider,
        std::span<MemoryRegionPolicyReport> memory_reports,
        ThreadPolicyTransaction* transaction = nullptr) noexcept;
    void stop() noexcept;

    [[nodiscard]] std::uint64_t arm(
        std::uint64_t absolute_deadline_ns,
        std::uint64_t timeout_ns) noexcept;
    [[nodiscard]] bool complete(
        std::uint64_t token,
        std::uint64_t finish_ns) noexcept;

private:
    [[nodiscard]] bool try_fire(
        std::uint64_t token,
        std::uint64_t now_ns) noexcept;
    void run() noexcept;
    static void thread_entry(void* context, std::size_t) noexcept;

    static constexpr std::uint64_t kFiredBit =
        std::uint64_t{1} << 63;
    static constexpr std::uint64_t kTokenMask = ~kFiredBit;

    OwnedThread thread_;
    std::mutex service_mutex_;
    std::condition_variable service_cv_;
    std::atomic<bool> started_{false};
    std::atomic<bool> startup_ready_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<std::uint64_t> active_state_{0};
    std::atomic<std::uint64_t> runtime_deadline_ns_{0};
    std::atomic<std::uint64_t> service_deadline_ns_{0};
    std::uint64_t next_token_ = 0;
    ThreadPolicyTransaction* startup_transaction_ = nullptr;
};

} // namespace rt::detail
