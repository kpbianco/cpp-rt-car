#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include <rt/runtime.hpp>

namespace {

using namespace std::chrono_literals;

class FakeClock final : public rt::RuntimeClock {
public:
    FakeClock(std::uint64_t current, std::uint64_t increment)
        : current_(current), increment_(increment) {}

    std::uint64_t now_ns() noexcept override {
        const auto value = current_;
        current_ += increment_;
        ++reads_;
        return value;
    }

    [[nodiscard]] std::uint64_t current() const noexcept { return current_; }
    [[nodiscard]] std::size_t reads() const noexcept { return reads_; }

private:
    std::uint64_t current_;
    std::uint64_t increment_;
    std::size_t reads_ = 0;
};

struct CallbackProbe {
    std::size_t calls = 0;
    std::uint64_t frame = 0;
    std::chrono::nanoseconds delta{0};
    std::size_t scratch_size = 0;
    std::byte* scratch_address = nullptr;
    std::byte initial_byte{};
    rt::NumericalMode numerical_mode = rt::NumericalMode::precise;
    double multiply_add = 0.0;
};

rt::CallbackResult probe_callback(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& probe = *static_cast<CallbackProbe*>(user_data);
    ++probe.calls;
    probe.frame = context.frame.frame_index;
    probe.delta = context.frame.delta;
    probe.scratch_size = context.scratch.size();
    probe.scratch_address = context.scratch.data();
    if (!context.scratch.empty()) {
        probe.initial_byte = context.scratch.front();
        context.scratch.front() = std::byte{0x5a};
    }
    probe.numerical_mode = context.numerics.mode();

    const double epsilon = std::ldexp(1.0, -27);
    probe.multiply_add =
        context.numerics.multiply_add(1.0 + epsilon, 1.0 - epsilon, -1.0);
    return rt::CallbackResult::ok;
}

rt::CallbackResult fail_callback(
    void* user_data,
    const rt::CallbackContext&) {
    auto& calls = *static_cast<std::size_t*>(user_data);
    ++calls;
    return rt::CallbackResult::error;
}

rt::CallbackResult throwing_callback(
    void*,
    const rt::CallbackContext&) {
    throw std::runtime_error("callback exception");
}

rt::CallbackResult count_callback(
    void* user_data,
    const rt::CallbackContext&) {
    ++*static_cast<std::size_t*>(user_data);
    return rt::CallbackResult::ok;
}

struct ReentrantStopProbe {
    rt::Runtime* runtime = nullptr;
    rt::Status observed = rt::Status::ok;
};

rt::CallbackResult reentrant_stop_callback(
    void* user_data,
    const rt::CallbackContext&) {
    auto& probe = *static_cast<ReentrantStopProbe*>(user_data);
    probe.observed = probe.runtime->stop();
    return rt::CallbackResult::ok;
}

} // namespace

TEST(HostRuntime, ReportsOnlyCompletedTargetPathCapabilities) {
    const auto capabilities = rt::query_capabilities();
    EXPECT_FALSE(capabilities.compiled_graph);
    EXPECT_TRUE(capabilities.host_driven_time);
    EXPECT_FALSE(capabilities.bounded_memory_plan);
}

TEST(HostRuntime, EnforcesLifecycleAndExecutesHostContext) {
    rt::Runtime runtime;
    CallbackProbe probe;
    rt::StepResult result;
    const rt::HostFrameContext frame{42, 2ms, std::nullopt};

    EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
    EXPECT_EQ(runtime.step(frame), rt::Status::invalid_state);
    EXPECT_EQ(runtime.start(), rt::Status::invalid_state);

    rt::RuntimeConfig config;
    config.callback_capacity = 2;
    config.scratch_bytes = 96;
    config.trace_capacity = 32;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback(
            {"probe", &probe_callback, &probe}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);

    EXPECT_EQ(
        runtime.register_callback(
            {"late", &probe_callback, &probe}),
        rt::Status::invalid_state);
    EXPECT_EQ(runtime.configure(config), rt::Status::invalid_state);
    EXPECT_EQ(runtime.step(frame), rt::Status::invalid_state);

    ASSERT_EQ(runtime.start(), rt::Status::ok);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::running);
    EXPECT_EQ(runtime.finalize(), rt::Status::invalid_state);
    EXPECT_EQ(
        runtime.step(rt::HostFrameContext{41, -1ns, std::nullopt}),
        rt::Status::invalid_argument);

    ASSERT_EQ(runtime.step(frame, &result), rt::Status::ok);
    EXPECT_EQ(result.callbacks_executed, 1u);
    EXPECT_EQ(probe.calls, 1u);
    EXPECT_EQ(probe.frame, 42u);
    EXPECT_EQ(probe.delta, 2ms);
    EXPECT_EQ(probe.scratch_size, 96u);

    ASSERT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::stopped);
    EXPECT_EQ(runtime.step(frame), rt::Status::invalid_state);
    EXPECT_EQ(runtime.start(), rt::Status::invalid_state);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);

    rt::Runtime finalized_runtime;
    ASSERT_EQ(finalized_runtime.finalize(), rt::Status::ok);
    EXPECT_EQ(finalized_runtime.stop(), rt::Status::ok);
    EXPECT_EQ(finalized_runtime.state(), rt::RuntimeState::stopped);
}

TEST(HostRuntime, StrictConfigurationKeysMapToBehavior) {
    rt::RuntimeConfig standalone;
    const auto unchanged = standalone;
    EXPECT_EQ(
        rt::set_runtime_config_value(standalone, "unknown_key", "1"),
        rt::Status::invalid_config);
    EXPECT_EQ(standalone.callback_capacity, unchanged.callback_capacity);
    EXPECT_EQ(
        rt::set_runtime_config_value(
            standalone,
            "callback_capacity",
            "2garbage"),
        rt::Status::invalid_config);

    rt::Runtime runtime;
    ASSERT_EQ(
        runtime.configure_key("callback_capacity", "1"),
        rt::Status::ok);
    ASSERT_EQ(runtime.configure_key("scratch_bytes", "7"), rt::Status::ok);
    ASSERT_EQ(runtime.configure_key("trace_capacity", "3"), rt::Status::ok);
    ASSERT_EQ(
        runtime.configure_key("numerical_mode", "fused_multiply_add"),
        rt::Status::ok);
    EXPECT_EQ(
        runtime.configure_key("worker_threads", "8"),
        rt::Status::invalid_config);

    CallbackProbe first;
    CallbackProbe second;
    ASSERT_EQ(
        runtime.register_callback({"first", &probe_callback, &first}),
        rt::Status::ok);
    EXPECT_EQ(
        runtime.register_callback({"second", &probe_callback, &second}),
        rt::Status::capacity_exceeded);

    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(
        runtime.step(rt::HostFrameContext{0, 1ns, std::nullopt}),
        rt::Status::ok);

    EXPECT_EQ(first.scratch_size, 7u);
    EXPECT_EQ(first.numerical_mode, rt::NumericalMode::fused_multiply_add);
    EXPECT_EQ(runtime.trace_event_count(), 3u);
    EXPECT_NE(first.multiply_add, 0.0);

    rt::RuntimeTraceEvent first_event;
    rt::RuntimeTraceEvent second_event;
    rt::RuntimeTraceEvent third_event;
    ASSERT_TRUE(runtime.trace_event(0, first_event));
    ASSERT_TRUE(runtime.trace_event(1, second_event));
    ASSERT_TRUE(runtime.trace_event(2, third_event));
    EXPECT_EQ(first_event.type, rt::RuntimeTraceEventType::callback_begin);
    EXPECT_EQ(second_event.type, rt::RuntimeTraceEventType::callback_end);
    EXPECT_EQ(third_event.type, rt::RuntimeTraceEventType::step_end);
    EXPECT_FALSE(runtime.trace_event(3, third_event));
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(HostRuntime, HostDrivenStepDoesNotPace) {
    rt::Runtime runtime;
    std::size_t calls = 0;
    ASSERT_EQ(
        runtime.register_callback(
            {"count", &count_callback, &calls}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    const auto begin = std::chrono::steady_clock::now();
    ASSERT_EQ(
        runtime.step(rt::HostFrameContext{0, 24h, std::nullopt}),
        rt::Status::ok);
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    EXPECT_EQ(calls, 1u);
    EXPECT_LT(elapsed, 250ms);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(HostRuntime, InstancesIsolateClockTraceNumericsAndScratch) {
    FakeClock first_clock(100, 10);
    FakeClock second_clock(10'000, 100);
    rt::Runtime first_runtime(first_clock);
    rt::Runtime second_runtime(second_clock);

    rt::RuntimeConfig first_config;
    first_config.scratch_bytes = 32;
    first_config.trace_capacity = 32;
    first_config.numerical_mode = rt::NumericalMode::precise;
    rt::RuntimeConfig second_config = first_config;
    second_config.scratch_bytes = 64;
    second_config.trace_capacity = 64;
    second_config.numerical_mode = rt::NumericalMode::fused_multiply_add;

    CallbackProbe first_probe;
    CallbackProbe second_probe;
    ASSERT_EQ(first_runtime.configure(first_config), rt::Status::ok);
    ASSERT_EQ(second_runtime.configure(second_config), rt::Status::ok);
    ASSERT_EQ(
        first_runtime.register_callback(
            {"first", &probe_callback, &first_probe}),
        rt::Status::ok);
    ASSERT_EQ(
        second_runtime.register_callback(
            {"second", &probe_callback, &second_probe}),
        rt::Status::ok);
    ASSERT_EQ(first_runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(second_runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(first_runtime.start(), rt::Status::ok);
    ASSERT_EQ(second_runtime.start(), rt::Status::ok);

    const auto second_reads_before_first_step = second_clock.reads();
    ASSERT_EQ(
        first_runtime.step(rt::HostFrameContext{1, 1ms, std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(second_clock.reads(), second_reads_before_first_step);
    const auto first_trace_count = first_runtime.trace_event_count();
    ASSERT_EQ(
        second_runtime.step(rt::HostFrameContext{2, 1ms, std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(first_runtime.trace_event_count(), first_trace_count);

    EXPECT_NE(first_probe.scratch_address, second_probe.scratch_address);
    EXPECT_EQ(first_probe.scratch_size, 32u);
    EXPECT_EQ(second_probe.scratch_size, 64u);
    EXPECT_EQ(first_probe.initial_byte, std::byte{0});
    EXPECT_EQ(second_probe.initial_byte, std::byte{0});
    EXPECT_EQ(first_probe.numerical_mode, rt::NumericalMode::precise);
    EXPECT_EQ(
        second_probe.numerical_mode,
        rt::NumericalMode::fused_multiply_add);
    EXPECT_EQ(first_probe.multiply_add, 0.0);
    EXPECT_NE(second_probe.multiply_add, 0.0);

    rt::RuntimeTraceEvent first_event;
    rt::RuntimeTraceEvent second_event;
    ASSERT_TRUE(first_runtime.trace_event(0, first_event));
    ASSERT_TRUE(second_runtime.trace_event(0, second_event));
    EXPECT_LT(first_event.timestamp_ns, second_event.timestamp_ns);

    EXPECT_EQ(first_runtime.stop(), rt::Status::ok);
    EXPECT_EQ(second_runtime.stop(), rt::Status::ok);
}

TEST(HostRuntime, CallbackFailureIsContainedAndFailFast) {
    FakeClock clock(100, 10);
    rt::Runtime runtime(clock);
    std::size_t failures = 0;
    std::size_t later_calls = 0;
    ASSERT_EQ(
        runtime.register_callback(
            {"fails", &fail_callback, &failures}),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback(
            {"later", &count_callback, &later_calls}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    rt::StepResult result;
    EXPECT_EQ(
        runtime.step(
            rt::HostFrameContext{0, 1ms, 50},
            &result),
        rt::Status::callback_failed);
    EXPECT_EQ(result.callbacks_executed, 1u);
    EXPECT_TRUE(result.deadline_missed);
    EXPECT_EQ(failures, 1u);
    EXPECT_EQ(later_calls, 0u);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::running);
    EXPECT_NE(runtime.last_error().find("fails"), std::string_view::npos);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(HostRuntime, CallbackExceptionsDoNotCrossBoundary) {
    rt::Runtime runtime;
    ASSERT_EQ(
        runtime.register_callback(
            {"throws", &throwing_callback, nullptr}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    EXPECT_EQ(
        runtime.step(rt::HostFrameContext{0, 1ms, std::nullopt}),
        rt::Status::callback_failed);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(HostRuntime, RejectsStopFromInsideCallback) {
    rt::Runtime runtime;
    ReentrantStopProbe probe{&runtime, rt::Status::ok};
    ASSERT_EQ(
        runtime.register_callback(
            {"reentrant-stop", &reentrant_stop_callback, &probe}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    EXPECT_EQ(
        runtime.step(rt::HostFrameContext{0, 1ms, std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(probe.observed, rt::Status::invalid_state);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::running);
    EXPECT_TRUE(runtime.last_error().empty());
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}
