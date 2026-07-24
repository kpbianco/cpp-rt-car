#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include <rt/runtime.hpp>

namespace {

constexpr std::size_t kUnsetWorker =
    std::numeric_limits<std::size_t>::max();

rt::CallbackResult no_op_phase(
    void*,
    const rt::CallbackContext&) {
    return rt::CallbackResult::ok;
}

struct AssignmentProbe {
    std::atomic<std::size_t> worker{kUnsetWorker};
    std::atomic<std::size_t> calls{0};
    std::atomic<std::size_t> mismatches{0};
};

rt::CallbackResult record_assignment(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& probe = *static_cast<AssignmentProbe*>(user_data);
    auto expected = kUnsetWorker;
    if (!probe.worker.compare_exchange_strong(
            expected,
            context.tasks.worker_index(),
            std::memory_order_acq_rel,
            std::memory_order_relaxed) &&
        expected != context.tasks.worker_index()) {
        probe.mismatches.fetch_add(1, std::memory_order_relaxed);
    }
    probe.calls.fetch_add(1, std::memory_order_relaxed);
    return rt::CallbackResult::ok;
}

rt::TaskResult count_range(
    void* user_data,
    const rt::TaskContext&,
    const rt::TaskRange&) {
    static_cast<std::atomic<std::size_t>*>(user_data)->fetch_add(
        1,
        std::memory_order_relaxed);
    return rt::TaskResult::ok;
}

struct QueueFullProbe {
    rt::Status status = rt::Status::ok;
    std::atomic<std::size_t> child_calls{0};
    std::chrono::steady_clock::duration elapsed{};
};

rt::CallbackResult force_queue_full(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& probe = *static_cast<QueueFullProbe*>(user_data);
    const auto begin = std::chrono::steady_clock::now();
    probe.status = context.tasks.parallel_for(
        4,
        1,
        &count_range,
        &probe.child_calls);
    probe.elapsed = std::chrono::steady_clock::now() - begin;
    return rt::CallbackResult::ok;
}

struct PhysicsProbe {
    explicit PhysicsProbe(std::size_t count, std::uint64_t multiplier_value)
        : values(count, 0),
          partials((count + 63) / 64, 0),
          multiplier(multiplier_value) {}

    std::vector<std::uint64_t> values;
    std::vector<std::uint64_t> partials;
    std::uint64_t multiplier;
    std::uint64_t current_value = 0;
    std::uint64_t reduced = 0;
    std::uint64_t last_frame = 0;
    std::byte* scratch_address = nullptr;
};

rt::TaskResult fill_physics_range(
    void* user_data,
    const rt::TaskContext&,
    const rt::TaskRange& range) {
    auto& probe = *static_cast<PhysicsProbe*>(user_data);
    for (std::size_t index = range.begin; index < range.end; ++index) {
        probe.values[index] = probe.current_value;
    }
    for (std::size_t spin = 0; spin < 512; ++spin) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    return rt::TaskResult::ok;
}

rt::TaskResult reduce_physics_range(
    void* user_data,
    const rt::TaskContext&,
    const rt::TaskRange& range) {
    auto& probe = *static_cast<PhysicsProbe*>(user_data);
    std::uint64_t sum = 0;
    for (std::size_t index = range.begin; index < range.end; ++index) {
        sum += probe.values[index];
    }
    probe.partials[range.task_index] = sum;
    return rt::TaskResult::ok;
}

rt::TaskResult combine_physics_range(
    void* user_data,
    const rt::TaskContext&,
    std::size_t left_task_index,
    std::size_t right_task_index) {
    auto& probe = *static_cast<PhysicsProbe*>(user_data);
    probe.partials[left_task_index] += probe.partials[right_task_index];
    return rt::TaskResult::ok;
}

rt::CallbackResult run_physics_phase(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& probe = *static_cast<PhysicsProbe*>(user_data);
    if (context.scratch.empty()) {
        return rt::CallbackResult::error;
    }
    if (probe.scratch_address != nullptr &&
        probe.scratch_address != context.scratch.data()) {
        return rt::CallbackResult::error;
    }
    probe.scratch_address = context.scratch.data();
    context.scratch.front() =
        static_cast<std::byte>(probe.multiplier);
    probe.current_value =
        (context.frame.frame_index + 1) * probe.multiplier;

    if (context.tasks.parallel_for(
            probe.values.size(),
            64,
            &fill_physics_range,
            &probe) != rt::Status::ok) {
        return rt::CallbackResult::error;
    }
    if (context.tasks.parallel_reduce(
            probe.values.size(),
            64,
            &reduce_physics_range,
            &combine_physics_range,
            &probe) != rt::Status::ok) {
        return rt::CallbackResult::error;
    }

    probe.reduced = probe.partials[0];
    probe.last_frame = context.frame.frame_index;
    return probe.reduced == probe.current_value * probe.values.size()
        ? rt::CallbackResult::ok
        : rt::CallbackResult::error;
}

struct BarrierProbe {
    std::array<PhysicsProbe*, 4> physics{};
    std::size_t calls = 0;
};

rt::CallbackResult verify_barrier(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& probe = *static_cast<BarrierProbe*>(user_data);
    for (std::size_t index = 0; index < probe.physics.size(); ++index) {
        const auto* physics = probe.physics[index];
        if (!physics ||
            physics->last_frame != context.frame.frame_index ||
            physics->reduced !=
                physics->current_value * physics->values.size()) {
            return rt::CallbackResult::error;
        }
        for (std::size_t other = index + 1;
             other < probe.physics.size();
             ++other) {
            if (physics->scratch_address ==
                probe.physics[other]->scratch_address) {
                return rt::CallbackResult::error;
            }
        }
    }
    ++probe.calls;
    return rt::CallbackResult::ok;
}

void run_nested_stress(rt::ExecutorPolicy policy) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 5;
    config.scratch_bytes = 32;
    config.trace_capacity = 0;
    config.executor_policy = policy;
    config.worker_count = 4;
    config.executor_queue_capacity = 512;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);

    std::array<std::unique_ptr<PhysicsProbe>, 4> physics{
        std::make_unique<PhysicsProbe>(16'384, 1),
        std::make_unique<PhysicsProbe>(256, 3),
        std::make_unique<PhysicsProbe>(512, 5),
        std::make_unique<PhysicsProbe>(768, 7),
    };
    std::array<rt::PhaseHandle, 4> phases{};
    std::array<rt::ResourceHandle, 4> resources{};
    BarrierProbe barrier{{
        physics[0].get(),
        physics[1].get(),
        physics[2].get(),
        physics[3].get(),
    }};
    rt::PhaseHandle barrier_phase;

    for (std::size_t index = 0; index < phases.size(); ++index) {
        const auto phase_name =
            std::array{"physics.0", "physics.1", "physics.2", "physics.3"};
        const auto resource_name =
            std::array{"state.0", "state.1", "state.2", "state.3"};
        ASSERT_EQ(
            runtime.register_callback(
                {
                    phase_name[index],
                    &run_physics_phase,
                    physics[index].get(),
                },
                phases[index]),
            rt::Status::ok);
        ASSERT_EQ(
            runtime.register_resource(
                resource_name[index],
                resources[index]),
            rt::Status::ok);
        ASSERT_EQ(
            runtime.declare_resource_access(
                phases[index],
                resources[index],
                rt::ResourceAccess::write),
            rt::Status::ok);
    }
    ASSERT_EQ(
        runtime.register_callback(
            {"barrier", &verify_barrier, &barrier},
            barrier_phase),
        rt::Status::ok);
    for (std::size_t index = 0; index < phases.size(); ++index) {
        ASSERT_EQ(
            runtime.add_dependency(phases[index], barrier_phase),
            rt::Status::ok);
        ASSERT_EQ(
            runtime.declare_resource_access(
                barrier_phase,
                resources[index],
                rt::ResourceAccess::read),
            rt::Status::ok);
    }

    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    for (std::uint64_t frame = 0; frame < 24; ++frame) {
        rt::StepResult result;
        ASSERT_EQ(
            runtime.step(
                rt::HostFrameContext{
                    frame,
                    std::chrono::microseconds(500),
                    std::nullopt,
                },
                &result),
            rt::Status::ok);
        ASSERT_EQ(result.callbacks_executed, 5u);
    }

    const auto stats = runtime.executor_stats();
    EXPECT_EQ(stats.policy, policy);
    EXPECT_EQ(stats.worker_count, 4u);
    EXPECT_EQ(stats.worker_starts, 4u);
    EXPECT_GT(stats.local_executions, 0u);
    EXPECT_EQ(stats.queue_full_rejections, 0u);
    if (policy == rt::ExecutorPolicy::bounded_throughput) {
        EXPECT_GT(stats.steal_attempts, 0u);
        EXPECT_GT(stats.successful_steals, 0u);
    } else {
        EXPECT_EQ(stats.steal_attempts, 0u);
        EXPECT_EQ(stats.successful_steals, 0u);
    }
    EXPECT_EQ(barrier.calls, 24u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

} // namespace

TEST(UnifiedExecutor, StaticAssignmentMetadataAndExecutionAreStable) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 8;
    config.scratch_bytes = 0;
    config.trace_capacity = 0;
    config.executor_policy = rt::ExecutorPolicy::static_deterministic;
    config.worker_count = 4;
    config.executor_queue_capacity = 8;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);

    std::array<AssignmentProbe, 8> probes{};
    std::array<rt::PhaseHandle, 8> phases{};
    for (std::size_t index = 0; index < phases.size(); ++index) {
        const auto names = std::array{
            "phase.0", "phase.1", "phase.2", "phase.3",
            "phase.4", "phase.5", "phase.6", "phase.7",
        };
        ASSERT_EQ(
            runtime.register_callback(
                {names[index], &record_assignment, &probes[index]},
                phases[index]),
            rt::Status::ok);
    }

    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    for (std::size_t index = 0; index < phases.size(); ++index) {
        rt::StaticPhaseAssignment assignment;
        ASSERT_TRUE(runtime.static_phase_assignment_at(index, assignment));
        EXPECT_EQ(assignment.phase, phases[index]);
        EXPECT_EQ(assignment.worker_index, index % config.worker_count);
    }

    ASSERT_EQ(runtime.start(), rt::Status::ok);
    for (std::uint64_t frame = 0; frame < 64; ++frame) {
        ASSERT_EQ(
            runtime.step(
                rt::HostFrameContext{
                    frame,
                    std::chrono::nanoseconds(1),
                    std::nullopt,
                }),
            rt::Status::ok);
    }
    for (std::size_t index = 0; index < probes.size(); ++index) {
        EXPECT_EQ(probes[index].worker.load(), index % config.worker_count);
        EXPECT_EQ(probes[index].calls.load(), 64u);
        EXPECT_EQ(probes[index].mismatches.load(), 0u);
    }

    const auto stats = runtime.executor_stats();
    EXPECT_EQ(stats.worker_starts, config.worker_count);
    EXPECT_EQ(stats.successful_steals, 0u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(UnifiedExecutor, QueueFullSubmissionReturnsWithinBound) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 1;
    config.scratch_bytes = 0;
    config.trace_capacity = 0;
    config.executor_policy = rt::ExecutorPolicy::static_deterministic;
    config.worker_count = 1;
    config.executor_queue_capacity = 2;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);

    QueueFullProbe probe;
    ASSERT_EQ(
        runtime.register_callback(
            {"queue-full", &force_queue_full, &probe}),
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

    EXPECT_EQ(probe.status, rt::Status::queue_full);
    EXPECT_EQ(probe.child_calls.load(), 2u);
    EXPECT_LT(probe.elapsed, std::chrono::milliseconds(100));
    const auto stats = runtime.executor_stats();
    EXPECT_EQ(stats.worker_count, 1u);
    EXPECT_EQ(stats.worker_starts, 1u);
    EXPECT_EQ(stats.queue_full_rejections, 1u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(UnifiedExecutor, FailFrameEscalatesIgnoredQueueRejection) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 1;
    config.scratch_bytes = 0;
    config.trace_capacity = 0;
    config.executor_policy = rt::ExecutorPolicy::static_deterministic;
    config.worker_count = 1;
    config.executor_queue_capacity = 2;
    config.task_scratch_slots = 8;
    config.overload_policy = rt::OverloadPolicy::fail_frame;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);

    QueueFullProbe probe;
    ASSERT_EQ(
        runtime.register_callback(
            {"queue-full-frame", &force_queue_full, &probe}),
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
        rt::Status::queue_full);

    EXPECT_EQ(probe.status, rt::Status::queue_full);
    EXPECT_EQ(probe.child_calls.load(), 2u);
    EXPECT_EQ(runtime.executor_stats().queue_full_rejections, 1u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(UnifiedExecutor, IndependentNestedWorkPassesStaticStress) {
    run_nested_stress(rt::ExecutorPolicy::static_deterministic);
}

TEST(UnifiedExecutor, ThroughputUsesLocalQueuesAndSuccessfulSteals) {
    run_nested_stress(rt::ExecutorPolicy::bounded_throughput);
}

TEST(UnifiedExecutor, RejectsInvalidOrUndersizedQueuePlans) {
    rt::RuntimeConfig config;
    EXPECT_EQ(
        rt::set_runtime_config_value(
            config,
            "executor_queue_capacity",
            "3"),
        rt::Status::invalid_config);
    EXPECT_EQ(
        rt::set_runtime_config_value(config, "worker_count", "0"),
        rt::Status::invalid_config);
    EXPECT_EQ(
        rt::set_runtime_config_value(
            config,
            "executor_policy",
            "unbounded"),
        rt::Status::invalid_config);

    rt::Runtime runtime;
    config.callback_capacity = 3;
    config.scratch_bytes = 0;
    config.trace_capacity = 0;
    config.worker_count = 1;
    config.executor_queue_capacity = 2;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback({"a", &no_op_phase, nullptr}),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback({"b", &no_op_phase, nullptr}),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback({"c", &no_op_phase, nullptr}),
        rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    EXPECT_NE(
        runtime.last_error().find("executor_queue_capacity"),
        std::string_view::npos);
}
