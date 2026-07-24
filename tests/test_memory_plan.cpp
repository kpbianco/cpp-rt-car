#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <rt/runtime.hpp>

namespace {

bool is_aligned(const void* pointer, std::size_t alignment) {
    return pointer != nullptr &&
           (reinterpret_cast<std::uintptr_t>(pointer) % alignment) == 0;
}

struct ContextProbe {
    std::size_t alignment = 0;
    std::size_t phase_bytes = 0;
    std::size_t task_bytes = 0;
    std::byte* phase_scratch = nullptr;
    std::byte* task_scratch = nullptr;
    std::atomic<std::size_t> errors{0};
    std::atomic<std::size_t> calls{0};
};

rt::CallbackResult record_context(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& probe = *static_cast<ContextProbe*>(user_data);
    const auto task_scratch = context.tasks.scratch();
    if (context.scratch.size() != probe.phase_bytes ||
        task_scratch.size() != probe.task_bytes ||
        !is_aligned(context.scratch.data(), probe.alignment) ||
        !is_aligned(task_scratch.data(), probe.alignment)) {
        probe.errors.fetch_add(1, std::memory_order_relaxed);
    }
    probe.phase_scratch = context.scratch.data();
    probe.task_scratch = task_scratch.data();
    probe.calls.fetch_add(1, std::memory_order_relaxed);
    return rt::CallbackResult::ok;
}

struct NestedScratchProbe {
    std::size_t expected_bytes = 0;
    std::size_t alignment = 0;
    std::byte* phase_task_scratch = nullptr;
    std::byte* child_scratch = nullptr;
    std::byte* grandchild_scratch = nullptr;
    rt::Status child_status = rt::Status::internal_error;
    rt::Status grandchild_status = rt::Status::internal_error;
    std::size_t calls = 0;
};

rt::TaskResult record_grandchild(
    void* user_data,
    const rt::TaskContext& context,
    const rt::TaskRange&) {
    auto& probe = *static_cast<NestedScratchProbe*>(user_data);
    const auto scratch = context.scratch();
    if (scratch.size() != probe.expected_bytes ||
        !is_aligned(scratch.data(), probe.alignment)) {
        return rt::TaskResult::error;
    }
    probe.grandchild_scratch = scratch.data();
    ++probe.calls;
    return rt::TaskResult::ok;
}

rt::TaskResult record_child(
    void* user_data,
    const rt::TaskContext& context,
    const rt::TaskRange&) {
    auto& probe = *static_cast<NestedScratchProbe*>(user_data);
    const auto scratch = context.scratch();
    if (scratch.size() != probe.expected_bytes ||
        !is_aligned(scratch.data(), probe.alignment)) {
        return rt::TaskResult::error;
    }
    probe.child_scratch = scratch.data();
    probe.grandchild_status = context.parallel_for(
        1,
        1,
        &record_grandchild,
        &probe);
    ++probe.calls;
    return probe.grandchild_status == rt::Status::ok
        ? rt::TaskResult::ok
        : rt::TaskResult::error;
}

rt::CallbackResult record_nested_scratch(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& probe = *static_cast<NestedScratchProbe*>(user_data);
    const auto scratch = context.tasks.scratch();
    if (scratch.size() != probe.expected_bytes ||
        !is_aligned(scratch.data(), probe.alignment)) {
        return rt::CallbackResult::error;
    }
    probe.phase_task_scratch = scratch.data();
    probe.child_status = context.tasks.parallel_for(
        1,
        1,
        &record_child,
        &probe);
    ++probe.calls;
    return probe.child_status == rt::Status::ok
        ? rt::CallbackResult::ok
        : rt::CallbackResult::error;
}

struct ScratchOverflowProbe {
    rt::Status status = rt::Status::internal_error;
    std::atomic<std::size_t> child_calls{0};
};

rt::TaskResult count_scratch_child(
    void* user_data,
    const rt::TaskContext& context,
    const rt::TaskRange&) {
    auto& probe = *static_cast<ScratchOverflowProbe*>(user_data);
    if (context.scratch().empty()) {
        return rt::TaskResult::error;
    }
    probe.child_calls.fetch_add(1, std::memory_order_relaxed);
    return rt::TaskResult::ok;
}

rt::CallbackResult ignore_scratch_overflow(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& probe = *static_cast<ScratchOverflowProbe*>(user_data);
    probe.status = context.tasks.parallel_for(
        2,
        1,
        &count_scratch_child,
        &probe);
    return rt::CallbackResult::ok;
}

rt::RuntimeConfig scratch_overflow_config(
    rt::OverloadPolicy policy) {
    rt::RuntimeConfig config;
    config.callback_capacity = 1;
    config.scratch_bytes = 16;
    config.trace_capacity = 0;
    config.worker_count = 1;
    config.executor_queue_capacity = 4;
    config.scratch_alignment = 64;
    config.task_scratch_bytes = 16;
    config.task_scratch_slots = 2;
    config.memory_budget_bytes = 1024 * 1024;
    config.overload_policy = policy;
    return config;
}

} // namespace

TEST(MemoryPlan, FinalizedPlanMatchesConfigurationAndAlignment) {
    rt::Runtime runtime;
    rt::MemoryPlan plan;
    EXPECT_FALSE(runtime.memory_plan(plan));

    rt::RuntimeConfig config;
    config.callback_capacity = 2;
    config.scratch_bytes = 17;
    config.trace_capacity = 10;
    config.worker_count = 2;
    config.executor_queue_capacity = 4;
    config.scratch_alignment = 64;
    config.task_scratch_bytes = 33;
    config.task_scratch_slots = 8;
    config.memory_budget_bytes = 1024 * 1024;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);

    std::array<ContextProbe, 2> probes{};
    for (auto& probe : probes) {
        probe.alignment = 64;
        probe.phase_bytes = 17;
        probe.task_bytes = 33;
    }
    rt::PhaseHandle first;
    rt::PhaseHandle second;
    ASSERT_EQ(
        runtime.register_callback(
            {"first", &record_context, &probes[0]},
            first),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback(
            {"second", &record_context, &probes[1]},
            second),
        rt::Status::ok);
    ASSERT_EQ(runtime.add_dependency(first, second), rt::Status::ok);
    std::array<std::byte, 17> replay_state{};
    ASSERT_EQ(
        runtime.register_state(
            {"memory-plan.state", 1, replay_state}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_TRUE(runtime.memory_plan(plan));

    EXPECT_EQ(plan.memory_budget_bytes, config.memory_budget_bytes);
    EXPECT_EQ(plan.phase_count, 2u);
    EXPECT_EQ(plan.phase_scratch_bytes, 17u);
    EXPECT_EQ(plan.phase_scratch_stride, 64u);
    EXPECT_EQ(plan.phase_scratch_total_bytes, 128u);
    EXPECT_EQ(plan.task_scratch_bytes, 33u);
    EXPECT_EQ(plan.task_scratch_stride, 64u);
    EXPECT_EQ(plan.task_scratch_slots, 8u);
    EXPECT_EQ(plan.task_scratch_total_bytes, 512u);
    EXPECT_GE(
        plan.trace_slot_bytes,
        sizeof(rt::RuntimeTraceEvent));
    EXPECT_EQ(
        plan.trace_storage_bytes,
        config.trace_capacity * plan.trace_slot_bytes);
    EXPECT_EQ(plan.state_count, 1u);
    EXPECT_EQ(
        plan.registered_state_bytes,
        replay_state.size());
    EXPECT_EQ(
        plan.snapshot_max_bytes,
        config.snapshot_max_bytes);
    EXPECT_EQ(
        plan.replay_input_capacity,
        config.replay_input_capacity);
    EXPECT_EQ(
        plan.input_log_max_bytes,
        config.input_log_max_bytes);
    EXPECT_EQ(plan.queue_slots, 8u);
    EXPECT_EQ(plan.scratch_alignment, 64u);
    EXPECT_EQ(
        plan.planned_bytes,
        plan.runtime_control_bytes +
            plan.executor_control_bytes +
            plan.phase_scratch_total_bytes +
            plan.task_scratch_total_bytes +
            plan.trace_storage_bytes);
    EXPECT_LE(plan.planned_bytes, plan.memory_budget_bytes);

    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(
        runtime.step(
            rt::HostFrameContext{
                0,
                std::chrono::nanoseconds(1),
                std::nullopt,
            }),
        rt::Status::ok);
    EXPECT_EQ(probes[0].errors.load(), 0u);
    EXPECT_EQ(probes[1].errors.load(), 0u);
    EXPECT_EQ(probes[0].calls.load(), 1u);
    EXPECT_EQ(probes[1].calls.load(), 1u);
    EXPECT_NE(probes[0].phase_scratch, probes[1].phase_scratch);
    EXPECT_NE(probes[0].task_scratch, probes[1].task_scratch);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(MemoryPlan, NestedExecutionContextsOwnDistinctScratch) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 1;
    config.scratch_bytes = 16;
    config.trace_capacity = 0;
    config.worker_count = 1;
    config.executor_queue_capacity = 4;
    config.scratch_alignment = 64;
    config.task_scratch_bytes = 32;
    config.task_scratch_slots = 4;
    config.memory_budget_bytes = 1024 * 1024;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);

    NestedScratchProbe probe;
    probe.expected_bytes = 32;
    probe.alignment = 64;
    ASSERT_EQ(
        runtime.register_callback(
            {"nested", &record_nested_scratch, &probe}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(
        runtime.step(
            rt::HostFrameContext{
                0,
                std::chrono::nanoseconds(1),
                std::nullopt,
            }),
        rt::Status::ok);

    EXPECT_EQ(probe.child_status, rt::Status::ok);
    EXPECT_EQ(probe.grandchild_status, rt::Status::ok);
    EXPECT_EQ(probe.calls, 3u);
    EXPECT_NE(probe.phase_task_scratch, probe.child_scratch);
    EXPECT_NE(probe.phase_task_scratch, probe.grandchild_scratch);
    EXPECT_NE(probe.child_scratch, probe.grandchild_scratch);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(MemoryPlan, RejectSubmissionReportsScratchExhaustion) {
    rt::Runtime runtime;
    ASSERT_EQ(
        runtime.configure(scratch_overflow_config(
            rt::OverloadPolicy::reject_submission)),
        rt::Status::ok);
    ScratchOverflowProbe probe;
    ASSERT_EQ(
        runtime.register_callback(
            {"overflow", &ignore_scratch_overflow, &probe}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    EXPECT_EQ(
        runtime.step(
            rt::HostFrameContext{
                0,
                std::chrono::nanoseconds(1),
                std::nullopt,
            }),
        rt::Status::ok);
    EXPECT_EQ(probe.status, rt::Status::scratch_exhausted);
    EXPECT_EQ(probe.child_calls.load(), 1u);
    EXPECT_EQ(runtime.executor_stats().scratch_exhaustions, 1u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(MemoryPlan, FailFrameEscalatesIgnoredScratchExhaustion) {
    rt::Runtime runtime;
    ASSERT_EQ(
        runtime.configure(scratch_overflow_config(
            rt::OverloadPolicy::fail_frame)),
        rt::Status::ok);
    ScratchOverflowProbe probe;
    ASSERT_EQ(
        runtime.register_callback(
            {"overflow", &ignore_scratch_overflow, &probe}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    EXPECT_EQ(
        runtime.step(
            rt::HostFrameContext{
                0,
                std::chrono::nanoseconds(1),
                std::nullopt,
            }),
        rt::Status::scratch_exhausted);
    EXPECT_EQ(probe.status, rt::Status::scratch_exhausted);
    EXPECT_EQ(probe.child_calls.load(), 1u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(MemoryPlan, RejectsInvalidOrUndersizedPlans) {
    rt::RuntimeConfig config;
    const auto unchanged = config;
    EXPECT_EQ(
        rt::set_runtime_config_value(
            config,
            "scratch_alignment",
            "3"),
        rt::Status::invalid_config);
    EXPECT_EQ(config.scratch_alignment, unchanged.scratch_alignment);
    EXPECT_EQ(
        rt::set_runtime_config_value(
            config,
            "task_scratch_slots",
            "0"),
        rt::Status::invalid_config);
    EXPECT_EQ(
        rt::set_runtime_config_value(
            config,
            "overload_policy",
            "heap_fallback"),
        rt::Status::invalid_config);

    rt::Runtime undersized;
    config.callback_capacity = 2;
    config.scratch_bytes = 0;
    config.trace_capacity = 0;
    config.worker_count = 1;
    config.executor_queue_capacity = 2;
    config.task_scratch_slots = 1;
    ASSERT_EQ(undersized.configure(config), rt::Status::ok);
    ASSERT_EQ(
        undersized.register_callback(
            {"first", &record_context, nullptr}),
        rt::Status::ok);
    ASSERT_EQ(
        undersized.register_callback(
            {"second", &record_context, nullptr}),
        rt::Status::ok);
    EXPECT_EQ(undersized.finalize(), rt::Status::invalid_config);
    EXPECT_NE(
        undersized.last_error().find("task_scratch_slots"),
        std::string_view::npos);

    rt::Runtime over_budget;
    config.task_scratch_slots = 2;
    config.memory_budget_bytes = 1;
    ASSERT_EQ(over_budget.configure(config), rt::Status::ok);
    EXPECT_EQ(over_budget.finalize(), rt::Status::invalid_config);
    EXPECT_NE(
        over_budget.last_error().find("memory_budget_bytes"),
        std::string_view::npos);
}
