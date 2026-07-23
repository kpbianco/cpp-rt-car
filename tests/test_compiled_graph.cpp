#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <rt/runtime.hpp>

namespace {

struct ExecutionProbe {
    std::uint32_t phase = 0;
    std::vector<std::uint32_t>* order = nullptr;
};

rt::CallbackResult record_phase(
    void* user_data,
    const rt::CallbackContext&) {
    auto& probe = *static_cast<ExecutionProbe*>(user_data);
    probe.order->push_back(probe.phase);
    return rt::CallbackResult::ok;
}

rt::CallbackResult no_op(
    void*,
    const rt::CallbackContext&) {
    return rt::CallbackResult::ok;
}

std::vector<std::uint32_t> reference_topological_order(
    std::size_t phase_count,
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& dependencies) {
    std::vector<std::vector<std::uint32_t>> successors(phase_count);
    std::vector<std::size_t> indegree(phase_count, 0);
    for (const auto& [prerequisite, dependent] : dependencies) {
        successors[prerequisite].push_back(dependent);
        ++indegree[dependent];
    }

    std::vector<bool> emitted(phase_count, false);
    std::vector<std::uint32_t> order;
    order.reserve(phase_count);
    while (order.size() != phase_count) {
        std::size_t ready = phase_count;
        for (std::size_t phase = 0; phase < phase_count; ++phase) {
            if (!emitted[phase] && indegree[phase] == 0) {
                ready = phase;
                break;
            }
        }
        if (ready == phase_count) {
            return {};
        }
        emitted[ready] = true;
        order.push_back(static_cast<std::uint32_t>(ready));
        for (const auto successor : successors[ready]) {
            --indegree[successor];
        }
    }
    return order;
}

} // namespace

TEST(CompiledGraph, CompilesAndExecutesDeterministicDependencyOrder) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 3;
    config.scratch_bytes = 0;
    config.trace_capacity = 0;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);

    std::vector<std::uint32_t> executed;
    executed.reserve(3);
    ExecutionProbe probes[] = {
        {0, &executed},
        {1, &executed},
        {2, &executed},
    };
    rt::PhaseHandle consumer;
    rt::PhaseHandle independent;
    rt::PhaseHandle producer;
    ASSERT_EQ(
        runtime.register_callback(
            {"consumer", &record_phase, &probes[0]},
            consumer),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback(
            {"independent", &record_phase, &probes[1]},
            independent),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback(
            {"producer", &record_phase, &probes[2]},
            producer),
        rt::Status::ok);

    rt::ResourceHandle state;
    ASSERT_EQ(runtime.register_resource("state", state), rt::Status::ok);
    ASSERT_EQ(
        runtime.add_dependency(producer, consumer),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.declare_resource_access(
            producer,
            state,
            rt::ResourceAccess::write),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.declare_resource_access(
            consumer,
            state,
            rt::ResourceAccess::read),
        rt::Status::ok);

    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    EXPECT_EQ(runtime.callback_count(), 3u);
    EXPECT_EQ(runtime.resource_count(), 1u);
    EXPECT_EQ(runtime.dependency_count(), 1u);
    EXPECT_EQ(runtime.resource_access_count(), 2u);

    rt::PhaseHandle compiled;
    ASSERT_TRUE(runtime.compiled_phase_at(0, compiled));
    EXPECT_EQ(compiled, independent);
    ASSERT_TRUE(runtime.compiled_phase_at(1, compiled));
    EXPECT_EQ(compiled, producer);
    ASSERT_TRUE(runtime.compiled_phase_at(2, compiled));
    EXPECT_EQ(compiled, consumer);
    EXPECT_FALSE(runtime.compiled_phase_at(3, compiled));
    EXPECT_FALSE(compiled.valid());

    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(
        runtime.step(
            rt::HostFrameContext{
                0,
                std::chrono::nanoseconds(1),
                std::nullopt,
            }),
        rt::Status::ok);
    EXPECT_EQ(
        executed,
        (std::vector<std::uint32_t>{1, 2, 0}));
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(CompiledGraph, RejectsCyclesBeforeStartWithPhaseDiagnostic) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 3;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);

    rt::PhaseHandle first;
    rt::PhaseHandle second;
    rt::PhaseHandle third;
    ASSERT_EQ(
        runtime.register_callback({"first", &no_op, nullptr}, first),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback({"second", &no_op, nullptr}, second),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback({"third", &no_op, nullptr}, third),
        rt::Status::ok);
    ASSERT_EQ(runtime.add_dependency(first, second), rt::Status::ok);
    ASSERT_EQ(runtime.add_dependency(second, third), rt::Status::ok);
    ASSERT_EQ(runtime.add_dependency(third, first), rt::Status::ok);

    EXPECT_EQ(runtime.finalize(), rt::Status::graph_cycle);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
    EXPECT_NE(runtime.last_error().find("cycle"), std::string_view::npos);
    EXPECT_NE(runtime.last_error().find("first"), std::string_view::npos);
    EXPECT_EQ(runtime.start(), rt::Status::invalid_state);

    rt::Runtime self_cycle;
    rt::PhaseHandle only;
    ASSERT_EQ(
        self_cycle.register_callback({"only", &no_op, nullptr}, only),
        rt::Status::ok);
    EXPECT_EQ(
        self_cycle.add_dependency(only, only),
        rt::Status::graph_cycle);

    rt::Runtime diagnostic_runtime;
    rt::PhaseHandle downstream;
    rt::PhaseHandle cycle_a;
    rt::PhaseHandle cycle_b;
    ASSERT_EQ(
        diagnostic_runtime.register_callback(
            {"downstream", &no_op, nullptr},
            downstream),
        rt::Status::ok);
    ASSERT_EQ(
        diagnostic_runtime.register_callback(
            {"cycle-a", &no_op, nullptr},
            cycle_a),
        rt::Status::ok);
    ASSERT_EQ(
        diagnostic_runtime.register_callback(
            {"cycle-b", &no_op, nullptr},
            cycle_b),
        rt::Status::ok);
    ASSERT_EQ(
        diagnostic_runtime.add_dependency(cycle_a, downstream),
        rt::Status::ok);
    ASSERT_EQ(
        diagnostic_runtime.add_dependency(cycle_a, cycle_b),
        rt::Status::ok);
    ASSERT_EQ(
        diagnostic_runtime.add_dependency(cycle_b, cycle_a),
        rt::Status::ok);
    ASSERT_EQ(
        diagnostic_runtime.finalize(),
        rt::Status::graph_cycle);
    EXPECT_EQ(
        diagnostic_runtime.last_error().find("downstream"),
        std::string_view::npos);
}

TEST(CompiledGraph, RejectsInvalidAndForeignHandles) {
    rt::Runtime first_runtime;
    rt::Runtime second_runtime;
    rt::PhaseHandle first_phase;
    rt::PhaseHandle second_phase;
    rt::ResourceHandle first_resource;
    rt::ResourceHandle second_resource;
    ASSERT_EQ(
        first_runtime.register_callback(
            {"first", &no_op, nullptr},
            first_phase),
        rt::Status::ok);
    ASSERT_EQ(
        second_runtime.register_callback(
            {"second", &no_op, nullptr},
            second_phase),
        rt::Status::ok);
    ASSERT_EQ(
        first_runtime.register_resource("first.resource", first_resource),
        rt::Status::ok);
    ASSERT_EQ(
        second_runtime.register_resource("second.resource", second_resource),
        rt::Status::ok);

    EXPECT_NE(first_phase.owner(), second_phase.owner());
    EXPECT_EQ(
        first_runtime.add_dependency(first_phase, second_phase),
        rt::Status::invalid_handle);
    EXPECT_EQ(
        first_runtime.add_dependency(
            rt::PhaseHandle{first_resource.value},
            first_phase),
        rt::Status::invalid_handle);
    EXPECT_EQ(
        first_runtime.add_dependency(
            rt::PhaseHandle{std::uint64_t{0}},
            first_phase),
        rt::Status::invalid_handle);
    EXPECT_EQ(
        first_runtime.declare_resource_access(
            first_phase,
            second_resource,
            rt::ResourceAccess::read),
        rt::Status::invalid_handle);
    EXPECT_EQ(
        first_runtime.declare_resource_access(
            second_phase,
            first_resource,
            rt::ResourceAccess::read),
        rt::Status::invalid_handle);
    EXPECT_EQ(
        first_runtime.declare_resource_access(
            first_phase,
            rt::ResourceHandle{first_phase.value},
            rt::ResourceAccess::read),
        rt::Status::invalid_handle);
    EXPECT_EQ(
        first_runtime.declare_resource_access(
            first_phase,
            first_resource,
            static_cast<rt::ResourceAccess>(99)),
        rt::Status::invalid_argument);
}

TEST(CompiledGraph, RequiresDependencyPathsForConflictingResourceAccess) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 3;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);

    rt::PhaseHandle writer;
    rt::PhaseHandle bridge;
    rt::PhaseHandle reader;
    rt::ResourceHandle state;
    ASSERT_EQ(
        runtime.register_callback({"writer", &no_op, nullptr}, writer),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback({"bridge", &no_op, nullptr}, bridge),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback({"reader", &no_op, nullptr}, reader),
        rt::Status::ok);
    ASSERT_EQ(runtime.register_resource("shared.state", state), rt::Status::ok);
    ASSERT_EQ(
        runtime.declare_resource_access(
            writer,
            state,
            rt::ResourceAccess::write),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.declare_resource_access(
            reader,
            state,
            rt::ResourceAccess::read),
        rt::Status::ok);

    EXPECT_EQ(runtime.finalize(), rt::Status::resource_conflict);
    EXPECT_NE(
        runtime.last_error().find("shared.state"),
        std::string_view::npos);
    EXPECT_NE(runtime.last_error().find("writer"), std::string_view::npos);
    EXPECT_NE(runtime.last_error().find("reader"), std::string_view::npos);

    ASSERT_EQ(runtime.add_dependency(writer, bridge), rt::Status::ok);
    ASSERT_EQ(runtime.add_dependency(bridge, reader), rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::ok);
}

TEST(CompiledGraph, AllowsUnorderedReadersAndRejectsDuplicateDeclarations) {
    rt::Runtime runtime;
    rt::PhaseHandle first;
    rt::PhaseHandle second;
    rt::ResourceHandle state;
    ASSERT_EQ(
        runtime.register_callback({"first", &no_op, nullptr}, first),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback({"second", &no_op, nullptr}, second),
        rt::Status::ok);
    ASSERT_EQ(runtime.register_resource("state", state), rt::Status::ok);
    ASSERT_EQ(
        runtime.declare_resource_access(
            first,
            state,
            rt::ResourceAccess::read),
        rt::Status::ok);
    EXPECT_EQ(
        runtime.declare_resource_access(
            first,
            state,
            rt::ResourceAccess::read),
        rt::Status::invalid_argument);
    ASSERT_EQ(
        runtime.declare_resource_access(
            second,
            state,
            rt::ResourceAccess::read),
        rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::ok);
}

TEST(CompiledGraph, OrdersWriteWriteConflictsExplicitly) {
    rt::Runtime runtime;
    rt::PhaseHandle first;
    rt::PhaseHandle second;
    rt::ResourceHandle state;
    ASSERT_EQ(
        runtime.register_callback({"first", &no_op, nullptr}, first),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback({"second", &no_op, nullptr}, second),
        rt::Status::ok);
    ASSERT_EQ(runtime.register_resource("state", state), rt::Status::ok);
    ASSERT_EQ(
        runtime.declare_resource_access(
            first,
            state,
            rt::ResourceAccess::write),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.declare_resource_access(
            second,
            state,
            rt::ResourceAccess::write),
        rt::Status::ok);

    EXPECT_EQ(runtime.finalize(), rt::Status::resource_conflict);
    ASSERT_EQ(runtime.add_dependency(second, first), rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::ok);
}

TEST(CompiledGraph, FreezesEveryTopologyMutationAtFinalization) {
    rt::Runtime runtime;
    rt::PhaseHandle phase;
    rt::ResourceHandle resource;
    ASSERT_EQ(
        runtime.register_callback({"phase", &no_op, nullptr}, phase),
        rt::Status::ok);
    ASSERT_EQ(runtime.register_resource("resource", resource), rt::Status::ok);

    rt::PhaseHandle compiled;
    EXPECT_FALSE(runtime.compiled_phase_at(0, compiled));
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_TRUE(runtime.compiled_phase_at(0, compiled));
    EXPECT_EQ(compiled, phase);

    rt::PhaseHandle late_phase{std::uint64_t{0}};
    rt::ResourceHandle late_resource{std::uint64_t{0}};
    EXPECT_EQ(
        runtime.register_callback(
            {"late", &no_op, nullptr},
            late_phase),
        rt::Status::invalid_state);
    EXPECT_FALSE(late_phase.valid());
    EXPECT_EQ(
        runtime.register_resource("late", late_resource),
        rt::Status::invalid_state);
    EXPECT_FALSE(late_resource.valid());
    EXPECT_EQ(
        runtime.add_dependency(phase, phase),
        rt::Status::invalid_state);
    EXPECT_EQ(
        runtime.declare_resource_access(
            phase,
            resource,
            rt::ResourceAccess::read),
        rt::Status::invalid_state);
}

TEST(CompiledGraph, RandomizedDagsAgreeWithReferenceExecutor) {
    std::mt19937 random(0x5eed1234u);
    constexpr std::size_t trial_count = 96;

    for (std::size_t trial = 0; trial < trial_count; ++trial) {
        SCOPED_TRACE(::testing::Message() << "trial " << trial);
        const std::size_t phase_count = 1 + (random() % 32);
        std::vector<std::uint32_t> hidden_order(phase_count);
        std::iota(
            hidden_order.begin(),
            hidden_order.end(),
            std::uint32_t{0});
        std::shuffle(hidden_order.begin(), hidden_order.end(), random);

        std::vector<std::pair<std::uint32_t, std::uint32_t>> dependencies;
        for (std::size_t left = 0; left < phase_count; ++left) {
            for (std::size_t right = left + 1;
                 right < phase_count;
                 ++right) {
                if ((random() % 5) == 0) {
                    dependencies.emplace_back(
                        hidden_order[left],
                        hidden_order[right]);
                }
            }
        }
        const auto expected =
            reference_topological_order(phase_count, dependencies);
        ASSERT_EQ(expected.size(), phase_count);

        rt::Runtime runtime;
        rt::RuntimeConfig config;
        config.callback_capacity = phase_count;
        config.scratch_bytes = 0;
        config.trace_capacity = 0;
        ASSERT_EQ(runtime.configure(config), rt::Status::ok);

        std::vector<std::uint32_t> executed;
        executed.reserve(phase_count);
        std::vector<ExecutionProbe> probes(phase_count);
        std::vector<rt::PhaseHandle> phases(phase_count);
        for (std::size_t phase = 0; phase < phase_count; ++phase) {
            probes[phase] = {
                static_cast<std::uint32_t>(phase),
                &executed,
            };
            const auto name = "phase." + std::to_string(phase);
            ASSERT_EQ(
                runtime.register_callback(
                    {name, &record_phase, &probes[phase]},
                    phases[phase]),
                rt::Status::ok);
        }
        for (const auto& [prerequisite, dependent] : dependencies) {
            ASSERT_EQ(
                runtime.add_dependency(
                    phases[prerequisite],
                    phases[dependent]),
                rt::Status::ok);
        }

        ASSERT_EQ(runtime.finalize(), rt::Status::ok);
        for (std::size_t index = 0; index < phase_count; ++index) {
            rt::PhaseHandle compiled;
            ASSERT_TRUE(runtime.compiled_phase_at(index, compiled));
            EXPECT_EQ(compiled.index(), expected[index]);
        }

        ASSERT_EQ(runtime.start(), rt::Status::ok);
        ASSERT_EQ(
            runtime.step(
                rt::HostFrameContext{
                    trial,
                    std::chrono::nanoseconds(1),
                    std::nullopt,
                }),
            rt::Status::ok);
        ASSERT_EQ(executed.size(), phase_count);
        std::vector<std::size_t> execution_position(
            phase_count,
            phase_count);
        for (std::size_t position = 0;
             position < executed.size();
             ++position) {
            ASSERT_LT(executed[position], phase_count);
            EXPECT_EQ(
                execution_position[executed[position]],
                phase_count);
            execution_position[executed[position]] = position;
        }
        for (const auto& [prerequisite, dependent] : dependencies) {
            EXPECT_LT(
                execution_position[prerequisite],
                execution_position[dependent]);
        }
        EXPECT_EQ(runtime.stop(), rt::Status::ok);
    }
}
