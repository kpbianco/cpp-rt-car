#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

#include <rt/runtime.hpp>

#include "rt/src/watchdog_monitor.hpp"

namespace {

using namespace std::chrono_literals;

class ManualClock final : public rt::RuntimeClock {
public:
    explicit ManualClock(std::uint64_t initial = 0) noexcept
        : current_(initial) {}

    std::uint64_t now_ns() noexcept override {
        return current_.load(std::memory_order_acquire);
    }

    rt::Status sleep_until_ns(
        std::uint64_t absolute_ns) noexcept override {
        sleep_targets_.push_back(absolute_ns);
        if (fail_sleep_at_ == sleep_targets_.size()) {
            return rt::Status::clock_failure;
        }
        auto current = current_.load(std::memory_order_relaxed);
        while (current < absolute_ns &&
               !current_.compare_exchange_weak(
                   current,
                   absolute_ns,
                   std::memory_order_release,
                   std::memory_order_relaxed)) {
        }
        return rt::Status::ok;
    }

    bool supports_absolute_sleep() const noexcept override {
        return true;
    }

    void advance(std::uint64_t duration_ns) noexcept {
        current_.fetch_add(duration_ns, std::memory_order_acq_rel);
    }

    void fail_sleep_at(std::size_t one_based_index) noexcept {
        fail_sleep_at_ = one_based_index;
    }

    const std::vector<std::uint64_t>& sleep_targets() const noexcept {
        return sleep_targets_;
    }

private:
    std::atomic<std::uint64_t> current_;
    std::vector<std::uint64_t> sleep_targets_;
    std::size_t fail_sleep_at_ = 0;
};

struct WorkProbe {
    ManualClock* clock = nullptr;
    std::uint64_t work_ns = 0;
    std::array<std::uint32_t, 8> observed_degradation{};
    std::size_t calls = 0;
};

rt::CallbackResult advance_clock(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& probe = *static_cast<WorkProbe*>(user_data);
    if (probe.calls < probe.observed_degradation.size()) {
        probe.observed_degradation[probe.calls] =
            context.degradation_level;
    }
    ++probe.calls;
    probe.clock->advance(probe.work_ns);
    return rt::CallbackResult::ok;
}

struct FrameObserver {
    std::array<rt::PeriodicFrameResult, 8> frames{};
    std::size_t count = 0;
    rt::Runtime* runtime = nullptr;
    rt::Status reentrant_step_status = rt::Status::ok;
};

rt::CallbackResult observe_frame(
    void* user_data,
    const rt::PeriodicFrameResult& frame) {
    auto& observer = *static_cast<FrameObserver*>(user_data);
    if (observer.count < observer.frames.size()) {
        observer.frames[observer.count] = frame;
    }
    ++observer.count;
    if (observer.runtime && observer.count == 1) {
        observer.reentrant_step_status = observer.runtime->step(
            rt::HostFrameContext{999, 1ns, std::nullopt});
    }
    return rt::CallbackResult::ok;
}

rt::Runtime make_runtime(
    ManualClock& clock,
    WorkProbe& work,
    std::uint64_t watchdog_timeout_ns = 0,
    std::uint32_t max_degradation = 0) {
    rt::Runtime runtime(clock);
    rt::RuntimeConfig config;
    config.callback_capacity = 1;
    config.worker_count = 1;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    config.trace_capacity = 64;
    config.watchdog_timeout_ns = watchdog_timeout_ns;
    config.watchdog_max_degradation_level = max_degradation;
    EXPECT_EQ(runtime.configure(config), rt::Status::ok);
    EXPECT_EQ(
        runtime.register_callback(
            {"periodic.work", &advance_clock, &work}),
        rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::ok);
    EXPECT_EQ(runtime.start(), rt::Status::ok);
    return runtime;
}

} // namespace

TEST(PeriodicRuntime, UsesAbsoluteEpochBasedReleasesAndDeadlines) {
    ManualClock clock;
    WorkProbe work{&clock, 10};
    auto runtime = make_runtime(clock, work);
    FrameObserver observer;
    observer.runtime = &runtime;

    rt::PeriodicRunConfig config;
    config.first_frame_index = 7;
    config.frame_count = 3;
    config.period = 50ns;
    config.first_release_ns = 100;
    config.relative_deadline = 40ns;
    rt::PeriodicRunResult result;

    ASSERT_EQ(
        runtime.run_periodic(
            config,
            &observe_frame,
            &observer,
            &result),
        rt::Status::ok);

    EXPECT_EQ(
        clock.sleep_targets(),
        (std::vector<std::uint64_t>{100, 150, 200}));
    ASSERT_EQ(observer.count, 3u);
    for (std::size_t index = 0; index < observer.count; ++index) {
        const auto expected_release =
            100u + static_cast<std::uint64_t>(index) * 50u;
        const auto& frame = observer.frames[index];
        EXPECT_EQ(frame.frame_index, 7u + index);
        EXPECT_EQ(frame.release_ns, expected_release);
        EXPECT_EQ(frame.wake_ns, expected_release);
        EXPECT_EQ(frame.start_ns, expected_release);
        EXPECT_EQ(frame.finish_ns, expected_release + 10u);
        EXPECT_EQ(frame.slack_ns, 30);
        EXPECT_FALSE(frame.deadline_missed);
    }
    EXPECT_EQ(observer.reentrant_step_status, rt::Status::invalid_state);
    EXPECT_EQ(result.frames_executed, 3u);
    EXPECT_EQ(result.deadline_misses, 0u);
    EXPECT_EQ(result.first_release_ns, 100u);
    EXPECT_EQ(result.next_release_ns, 250u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(PeriodicRuntime, LateFramesDoNotShiftTheReleaseEpoch) {
    ManualClock clock;
    WorkProbe work{&clock, 70};
    auto runtime = make_runtime(clock, work);
    FrameObserver observer;

    rt::PeriodicRunConfig config;
    config.frame_count = 3;
    config.period = 50ns;
    config.first_release_ns = 100;
    config.relative_deadline = 40ns;
    rt::PeriodicRunResult result;

    ASSERT_EQ(
        runtime.run_periodic(
            config,
            &observe_frame,
            &observer,
            &result),
        rt::Status::ok);

    EXPECT_EQ(
        clock.sleep_targets(),
        (std::vector<std::uint64_t>{100, 150, 200}));
    ASSERT_EQ(observer.count, 3u);
    EXPECT_EQ(observer.frames[0].wake_ns, 100u);
    EXPECT_EQ(observer.frames[1].wake_ns, 170u);
    EXPECT_EQ(observer.frames[2].wake_ns, 240u);
    EXPECT_EQ(observer.frames[0].slack_ns, -30);
    EXPECT_EQ(observer.frames[1].slack_ns, -50);
    EXPECT_EQ(observer.frames[2].slack_ns, -70);
    EXPECT_EQ(result.deadline_misses, 3u);
    EXPECT_EQ(result.next_release_ns, 250u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(PeriodicRuntime, WatchdogIsOneShotAndDegradesOnTheFrameThread) {
    ManualClock clock;
    WorkProbe work{&clock, 25};
    auto runtime = make_runtime(clock, work, 20, 2);
    FrameObserver observer;

    rt::PeriodicRunConfig config;
    config.frame_count = 3;
    config.period = 100ns;
    config.first_release_ns = 100;
    config.relative_deadline = 80ns;
    rt::PeriodicRunResult result;

    ASSERT_EQ(
        runtime.run_periodic(
            config,
            &observe_frame,
            &observer,
            &result),
        rt::Status::ok);

    ASSERT_EQ(work.calls, 3u);
    EXPECT_EQ(work.observed_degradation[0], 0u);
    EXPECT_EQ(work.observed_degradation[1], 1u);
    EXPECT_EQ(work.observed_degradation[2], 2u);
    EXPECT_EQ(result.watchdog_events, 3u);
    EXPECT_EQ(result.final_degradation_level, 2u);
    EXPECT_EQ(runtime.degradation_level(), 2u);
    for (std::size_t index = 0; index < 3; ++index) {
        EXPECT_TRUE(observer.frames[index].watchdog_fired);
    }

    std::size_t watchdog_trace_events = 0;
    std::size_t degradation_trace_events = 0;
    for (std::size_t index = 0;
         index < runtime.trace_event_count();
         ++index) {
        rt::RuntimeTraceEvent event;
        ASSERT_TRUE(runtime.trace_event(index, event));
        watchdog_trace_events +=
            event.type == rt::RuntimeTraceEventType::watchdog_fired;
        degradation_trace_events +=
            event.type ==
            rt::RuntimeTraceEventType::degradation_applied;
    }
    EXPECT_EQ(watchdog_trace_events, 3u);
    EXPECT_EQ(degradation_trace_events, 2u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(PeriodicRuntime, WatchdogServiceDetectsExpiryWithoutRuntimeClockAdvance) {
    rt::detail::NativeThreadPolicyProvider provider;
    rt::detail::ThreadStartupGate gate;
    rt::ThreadPolicy requested;
    rt::ThreadPolicy role_default;
    role_default.wait_strategy = rt::WaitStrategy::park;
    rt::ThreadPolicy resolved;
    rt::PolicyResolutionState resolution{};
    std::int32_t system_error = 0;
    ASSERT_EQ(
        provider.resolve(
            rt::thread_role_watchdog,
            rt::PolicyApplicationMode::apply_and_verify,
            true,
            true,
            false,
            requested,
            role_default,
            resolved,
            resolution,
            system_error),
        rt::Status::ok);
    rt::detail::ThreadRolePlan plan{
        rt::thread_role_watchdog,
        requested,
        resolved,
        resolution};
    rt::detail::WatchdogMonitor watchdog;
    ASSERT_EQ(watchdog.start(provider, gate, plan), rt::Status::ok);
    gate.commit();
    watchdog.wait_started();
    const auto token = watchdog.arm(
        std::numeric_limits<std::uint64_t>::max(),
        5'000'000);
    ASSERT_NE(token, 0u);
    const auto observation_deadline =
        std::chrono::steady_clock::now() + 5s;
    while (!watchdog.has_fired(token) &&
           std::chrono::steady_clock::now() < observation_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(watchdog.has_fired(token));
    EXPECT_TRUE(watchdog.complete(token, 0));
    EXPECT_FALSE(watchdog.complete(token, 0));
    watchdog.stop();
}

TEST(PeriodicRuntime, PropagatesClockFailureWithoutExecutingAFrame) {
    ManualClock clock;
    clock.fail_sleep_at(1);
    WorkProbe work{&clock, 1};
    auto runtime = make_runtime(clock, work);
    rt::PeriodicRunConfig config;
    config.frame_count = 2;
    config.period = 100ns;
    config.first_release_ns = 10;
    config.relative_deadline = 100ns;
    rt::PeriodicRunResult result;

    EXPECT_EQ(
        runtime.run_periodic(config, nullptr, nullptr, &result),
        rt::Status::clock_failure);
    EXPECT_EQ(result.frames_executed, 0u);
    EXPECT_EQ(work.calls, 0u);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::running);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(PeriodicRuntime, RejectsInvalidAndOverflowingSchedules) {
    ManualClock clock;
    WorkProbe work{&clock, 0};
    auto runtime = make_runtime(clock, work);
    rt::PeriodicRunConfig config;
    config.frame_count = 0;
    EXPECT_EQ(
        runtime.run_periodic(config),
        rt::Status::invalid_argument);

    config.frame_count = 2;
    config.period = 2ns;
    config.first_release_ns =
        std::numeric_limits<std::uint64_t>::max() - 1;
    config.relative_deadline = 1ns;
    EXPECT_EQ(
        runtime.run_periodic(config),
        rt::Status::invalid_argument);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}
