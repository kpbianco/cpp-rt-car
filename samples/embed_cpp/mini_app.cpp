#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>

#include <rt/runtime.hpp>

namespace {

struct SampleState {
    std::uint64_t produced = 0;
    std::uint64_t consumed = 0;
    std::uint64_t last_frame = 0;
    std::array<std::uint64_t, 64> lanes{};
};

rt::TaskResult produce_range(
    void* user_data,
    const rt::TaskContext& context,
    const rt::TaskRange& range) {
    auto& state = *static_cast<SampleState*>(user_data);
    const auto scratch = context.scratch();
    if (scratch.size() < sizeof(std::uint64_t) ||
        (reinterpret_cast<std::uintptr_t>(scratch.data()) % 64u) != 0u) {
        return rt::TaskResult::error;
    }
    scratch.front() = std::byte{0x17};
    for (std::size_t index = range.begin; index < range.end; ++index) {
        state.lanes[index] = state.produced;
    }
    return rt::TaskResult::ok;
}

rt::CallbackResult produce(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& state = *static_cast<SampleState*>(user_data);
    if (context.frame.delta != std::chrono::milliseconds(2) ||
        context.scratch.size() < sizeof(std::uint64_t)) {
        return rt::CallbackResult::error;
    }

    ++state.produced;
    state.last_frame = context.frame.frame_index;
    context.scratch.front() = std::byte{0x2a};
    return context.tasks.parallel_for(
               state.lanes.size(),
               8,
               &produce_range,
               &state) == rt::Status::ok
        ? rt::CallbackResult::ok
        : rt::CallbackResult::error;
}

rt::CallbackResult consume(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& state = *static_cast<SampleState*>(user_data);
    if (context.frame.delta != std::chrono::milliseconds(2) ||
        context.scratch.empty() ||
        state.produced != state.consumed + 1) {
        return rt::CallbackResult::error;
    }
    for (const auto lane : state.lanes) {
        if (lane != state.produced) {
            return rt::CallbackResult::error;
        }
    }
    ++state.consumed;
    return rt::CallbackResult::ok;
}

bool check(
    const rt::Runtime& runtime,
    rt::Status status,
    const char* operation) {
    if (status == rt::Status::ok) {
        return true;
    }
    std::cerr << "mini_app_cpp: " << operation << " failed: "
              << runtime.last_error() << '\n';
    return false;
}

} // namespace

int main() {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 2;
    config.scratch_bytes = 128;
    config.trace_capacity = 32;
    config.executor_policy = rt::ExecutorPolicy::bounded_throughput;
    config.worker_count = 4;
    config.executor_queue_capacity = 128;
    config.scratch_alignment = 64;
    config.task_scratch_bytes = 64;
    config.task_scratch_slots = 128;
    config.memory_budget_bytes = 1024 * 1024;
    config.overload_policy = rt::OverloadPolicy::fail_frame;
    if (rt::set_runtime_config_value(
            config,
            "workload_id",
            "sample.embed_cpp") != rt::Status::ok) {
        return 1;
    }

    SampleState state;
    rt::PhaseHandle consumer;
    rt::PhaseHandle producer;
    rt::ResourceHandle simulation_state;
    if (!check(runtime, runtime.configure(config), "configure") ||
        !check(
            runtime,
            runtime.register_callback(
                {"sample.consume", &consume, &state},
                consumer),
            "register consumer") ||
        !check(
            runtime,
            runtime.register_callback(
                {"sample.produce", &produce, &state},
                producer),
            "register producer") ||
        !check(
            runtime,
            runtime.register_resource(
                "sample.simulation-state",
                simulation_state),
            "register resource") ||
        !check(
            runtime,
            runtime.add_dependency(producer, consumer),
            "add dependency") ||
        !check(
            runtime,
            runtime.declare_resource_access(
                producer,
                simulation_state,
                rt::ResourceAccess::write),
            "declare producer access") ||
        !check(
            runtime,
            runtime.declare_resource_access(
                consumer,
                simulation_state,
                rt::ResourceAccess::read),
            "declare consumer access") ||
        !check(runtime, runtime.finalize(), "finalize")) {
        return 1;
    }

    rt::MemoryPlan memory_plan;
    if (!runtime.memory_plan(memory_plan) ||
        memory_plan.planned_bytes > memory_plan.memory_budget_bytes ||
        memory_plan.task_scratch_bytes != config.task_scratch_bytes ||
        memory_plan.overload_policy != config.overload_policy ||
        !check(runtime, runtime.start(), "start")) {
        return 1;
    }

    rt::PeriodicRunConfig periodic;
    periodic.frame_count = 5;
    periodic.period = std::chrono::milliseconds(2);
    periodic.relative_deadline = std::chrono::seconds(1);
    rt::PeriodicRunResult periodic_result;
    if (!check(
            runtime,
            runtime.run_periodic(
                periodic,
                nullptr,
                nullptr,
                &periodic_result),
            "run periodic") ||
        periodic_result.frames_executed != 5 ||
        periodic_result.deadline_misses != 0 ||
        periodic_result.watchdog_events != 0) {
        return 1;
    }

    if (!check(runtime, runtime.stop(), "stop") ||
        state.produced != 5 ||
        state.consumed != 5 ||
        state.last_frame != 4) {
        return 1;
    }

    rt::RuntimeMetricSnapshot metrics;
    rt::RuntimeTraceCursor trace_cursor;
    rt::RuntimeTraceReadResult trace_result;
    std::array<rt::RuntimeTraceEvent, 32> trace_events{};
    if (!check(
            runtime,
            runtime.metrics_snapshot(
                rt::RuntimeMetricWindow::cumulative,
                nullptr,
                metrics),
            "read metrics") ||
        metrics.samples[static_cast<std::size_t>(
            rt::RuntimeMetricId::frames_completed)].value != 5 ||
        metrics.samples[static_cast<std::size_t>(
            rt::RuntimeMetricId::callbacks_completed)].value != 10 ||
        !check(
            runtime,
            runtime.read_trace(
                trace_cursor,
                trace_events,
                trace_result),
            "read trace") ||
        trace_result.events_read == 0 ||
        std::string_view(
            metrics.metadata.workload_id.data()) !=
            "sample.embed_cpp") {
        return 1;
    }

    std::cout << "mini_app_cpp: executed 5 absolute-cadence graph frames with "
              << memory_plan.planned_bytes
              << " planned runtime bytes; observability schema "
              << metrics.metadata.schema_version
              << " retained "
              << trace_result.events_read
              << " events\n";
    return 0;
}
