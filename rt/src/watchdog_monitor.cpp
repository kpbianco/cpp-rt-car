#include "watchdog_monitor.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <new>

#include <rt/arch.hpp>

namespace rt::detail {

namespace {

constexpr auto kMaximumParkInterval = std::chrono::milliseconds(1);

} // namespace

WatchdogMonitor::~WatchdogMonitor() {
    (void)stop();
}

Status WatchdogMonitor::start(
    ThreadPolicyProvider& provider,
    ThreadStartupGate& gate,
    const ThreadRolePlan& plan) noexcept {
    if (started_.load(std::memory_order_acquire)) {
        return Status::invalid_state;
    }

    stop_requested_.store(false, std::memory_order_release);
    active_state_.store(0, std::memory_order_release);
    wait_strategy_ = plan.resolved.wait_strategy;
    started_.store(true, std::memory_order_release);
    const auto status = thread_.start(
        provider,
        gate,
        plan,
        0,
        startup_result_,
        &WatchdogMonitor::run_entry,
        this);
    if (status != Status::ok) {
        started_.store(false, std::memory_order_release);
    }
    return status;
}

Status WatchdogMonitor::stop() noexcept {
    (void)started_.exchange(false, std::memory_order_acq_rel);
    stop_requested_.store(true, std::memory_order_release);
    active_state_.store(0, std::memory_order_release);
    service_cv_.notify_one();
    return thread_.cleanup_and_join();
}

void WatchdogMonitor::wait_started() const noexcept {
    thread_.wait_started();
}

std::uint64_t WatchdogMonitor::arm(
    std::uint64_t absolute_deadline_ns,
    std::uint64_t timeout_ns) noexcept {
    if (!started_.load(std::memory_order_acquire)) {
        return 0;
    }

    next_token_ = (next_token_ + 1) & kTokenMask;
    if (next_token_ == 0) {
        next_token_ = 1;
    }
    const auto service_now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const auto service_deadline =
        timeout_ns > std::numeric_limits<std::uint64_t>::max() - service_now
        ? std::numeric_limits<std::uint64_t>::max()
        : service_now + timeout_ns;
    runtime_deadline_ns_.store(
        absolute_deadline_ns,
        std::memory_order_relaxed);
    service_deadline_ns_.store(
        service_deadline,
        std::memory_order_relaxed);
    active_state_.store(next_token_, std::memory_order_release);
    service_cv_.notify_one();
    return next_token_;
}

bool WatchdogMonitor::try_fire(
    std::uint64_t token,
    std::uint64_t now_ns) noexcept {
    if (token == 0 ||
        now_ns < runtime_deadline_ns_.load(std::memory_order_acquire)) {
        return false;
    }

    auto expected = token;
    return active_state_.compare_exchange_strong(
        expected,
        token | kFiredBit,
        std::memory_order_acq_rel,
        std::memory_order_acquire);
}

bool WatchdogMonitor::complete(
    std::uint64_t token,
    std::uint64_t finish_ns) noexcept {
    if (token == 0) {
        return false;
    }

    (void)try_fire(token, finish_ns);
    auto state = active_state_.load(std::memory_order_acquire);
    bool fired = false;
    while (state == token || state == (token | kFiredBit)) {
        fired = fired || state == (token | kFiredBit);
        if (active_state_.compare_exchange_weak(
                state,
                0,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            break;
        }
    }
    service_cv_.notify_one();
    return fired;
}

bool WatchdogMonitor::has_fired(std::uint64_t token) const noexcept {
    return token != 0 &&
        active_state_.load(std::memory_order_acquire) ==
            (token | kFiredBit);
}

void WatchdogMonitor::run() noexcept {
    if (wait_strategy_ != WaitStrategy::park) {
        while (!stop_requested_.load(std::memory_order_acquire)) {
            const auto state = active_state_.load(std::memory_order_acquire);
            if (state != 0 && (state & kFiredBit) == 0) {
                const auto deadline =
                    service_deadline_ns_.load(std::memory_order_acquire);
                const auto now = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                if (now >= deadline) {
                    auto expected = state;
                    (void)active_state_.compare_exchange_strong(
                        expected,
                        state | kFiredBit,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire);
                    continue;
                }
            }
            if (wait_strategy_ == WaitStrategy::spin) {
                rt::cpu_relax();
            } else {
                std::this_thread::yield();
            }
        }
        return;
    }
    std::unique_lock<std::mutex> lock(service_mutex_);
    std::uint64_t observed_state = 0;
    while (!stop_requested_.load(std::memory_order_acquire)) {
        const auto state =
            active_state_.load(std::memory_order_acquire);
        if (state == 0 || (state & kFiredBit) != 0) {
            observed_state = state;
            service_cv_.wait_for(lock, kMaximumParkInterval, [&] {
                return stop_requested_.load(std::memory_order_acquire) ||
                    active_state_.load(std::memory_order_acquire) !=
                        observed_state;
            });
            continue;
        }

        const auto deadline =
            service_deadline_ns_.load(std::memory_order_acquire);
        const auto now = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        if (now >= deadline) {
            auto expected = state;
            (void)active_state_.compare_exchange_strong(
                expected,
                state | kFiredBit,
                std::memory_order_acq_rel,
                std::memory_order_acquire);
            continue;
        }

        const auto remaining = deadline - now;
        const auto bounded =
            std::min<std::uint64_t>(
                remaining,
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        kMaximumParkInterval).count()));
        service_cv_.wait_for(
            lock,
            std::chrono::nanoseconds(bounded),
            [&] {
                return stop_requested_.load(std::memory_order_acquire) ||
                    active_state_.load(std::memory_order_acquire) != state;
            });
    }
}

void WatchdogMonitor::run_entry(void* monitor) noexcept {
    static_cast<WatchdogMonitor*>(monitor)->run();
}

} // namespace rt::detail
