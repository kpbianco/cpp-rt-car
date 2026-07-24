#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "core/units.hpp"
#include "rt/graph.hpp"
#include "rt/version.hpp"

class SimCore;

namespace rt {

// Capabilities report only completed target-path guarantees. A false value
// identifies a later roadmap milestone rather than a disabled build option.
struct Capabilities {
    bool compiled_graph;
    bool host_driven_time;
    bool unified_cpu_executor;
    bool bounded_memory_plan;
};

// Query runtime capabilities
Capabilities query_capabilities() noexcept;

// Example function using strong types
core::seconds tick_duration(core::seconds dt) noexcept;

inline constexpr std::uint32_t runtime_config_schema_version = 3;

enum class RuntimeState : std::uint8_t {
    configuring,
    finalized,
    running,
    stopped,
};

enum class Status : std::int32_t {
    ok = 0,
    invalid_argument = -1,
    invalid_state = -2,
    invalid_config = -3,
    capacity_exceeded = -4,
    callback_failed = -5,
    resource_exhausted = -6,
    internal_error = -7,
    invalid_handle = -8,
    graph_cycle = -9,
    resource_conflict = -10,
    queue_full = -11,
    scratch_exhausted = -12,
};

[[nodiscard]] const char* status_message(Status status) noexcept;

enum class NumericalMode : std::uint8_t {
    precise,
    fused_multiply_add,
};

enum class ExecutorPolicy : std::uint8_t {
    static_deterministic,
    bounded_throughput,
};

enum class OverloadPolicy : std::uint8_t {
    reject_submission,
    fail_frame,
};

enum class TaskResult : std::uint8_t {
    ok,
    error,
};

namespace detail {
class Executor;
}

class TaskContext;

struct TaskRange {
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t task_index = 0;
};

using RangeTaskCallback = TaskResult (*)(
    void* user_data,
    const TaskContext& context,
    const TaskRange& range);

using ReductionTaskCallback = TaskResult (*)(
    void* user_data,
    const TaskContext& context,
    std::size_t left_task_index,
    std::size_t right_task_index);

// A TaskContext is valid only while its callback is running. Nested work is
// synchronous: these methods return only after every accepted child finishes.
class TaskContext {
public:
    [[nodiscard]] Status parallel_for(
        std::size_t item_count,
        std::size_t grain_size,
        RangeTaskCallback callback,
        void* user_data = nullptr) const noexcept;
    [[nodiscard]] Status parallel_reduce(
        std::size_t item_count,
        std::size_t grain_size,
        RangeTaskCallback range_callback,
        ReductionTaskCallback combine_callback,
        void* user_data = nullptr) const noexcept;

    [[nodiscard]] std::size_t worker_index() const noexcept {
        return worker_index_;
    }
    [[nodiscard]] std::size_t phase_index() const noexcept {
        return phase_index_;
    }
    [[nodiscard]] std::size_t task_index() const noexcept {
        return task_index_;
    }
    // This callback-local block is reserved before the task is accepted,
    // remains exclusively owned until the callback returns, and has
    // unspecified contents on entry.
    [[nodiscard]] std::span<std::byte> scratch() const noexcept {
        return scratch_;
    }

private:
    TaskContext(
        detail::Executor& executor,
        std::size_t worker_index,
        std::size_t phase_index,
        std::size_t task_index,
        std::span<std::byte> scratch) noexcept
        : executor_(&executor),
          worker_index_(worker_index),
          phase_index_(phase_index),
          task_index_(task_index),
          scratch_(scratch) {}

    detail::Executor* executor_ = nullptr;
    std::size_t worker_index_ = 0;
    std::size_t phase_index_ = 0;
    std::size_t task_index_ = 0;
    std::span<std::byte> scratch_{};

    friend class detail::Executor;
};

struct RuntimeConfig {
    std::size_t callback_capacity = 64;
    std::size_t scratch_bytes = 64 * 1024;
    std::size_t trace_capacity = 1024;
    NumericalMode numerical_mode = NumericalMode::precise;
    ExecutorPolicy executor_policy = ExecutorPolicy::static_deterministic;
    std::size_t worker_count = 1;
    std::size_t executor_queue_capacity = 1024;
    std::size_t scratch_alignment = 64;
    std::size_t task_scratch_bytes = 4 * 1024;
    std::size_t task_scratch_slots = 1024;
    std::size_t memory_budget_bytes = 256 * 1024 * 1024;
    OverloadPolicy overload_policy = OverloadPolicy::reject_submission;
};

// Applies one strict schema key to a typed configuration. The supported keys
// are callback_capacity, scratch_bytes, trace_capacity, numerical_mode,
// executor_policy, worker_count, executor_queue_capacity, scratch_alignment,
// task_scratch_bytes, task_scratch_slots, memory_budget_bytes, and
// overload_policy. Unknown keys and partially parsed values are rejected.
[[nodiscard]] Status set_runtime_config_value(
    RuntimeConfig& config,
    std::string_view key,
    std::string_view value) noexcept;

class RuntimeClock {
public:
    virtual ~RuntimeClock() = default;
    // Values must be monotonic nanoseconds in one clock domain. A Runtime
    // constructed with an injected clock borrows it for the Runtime lifetime.
    [[nodiscard]] virtual std::uint64_t now_ns() noexcept = 0;
};

class NumericalPolicy {
public:
    explicit NumericalPolicy(
        NumericalMode mode = NumericalMode::precise) noexcept
        : mode_(mode) {}

    [[nodiscard]] NumericalMode mode() const noexcept { return mode_; }
    [[nodiscard]] double multiply_add(double a, double b, double c) const noexcept;

private:
    NumericalMode mode_;
};

struct HostFrameContext {
    std::uint64_t frame_index = 0;
    std::chrono::nanoseconds delta{0};
    std::optional<std::uint64_t> deadline_ns{};
};

struct CallbackContext {
    const HostFrameContext& frame;
    // Valid only for this phase callback. Each phase owns a distinct block so
    // independent phases cannot race through runtime-provided scratch.
    std::span<std::byte> scratch;
    const NumericalPolicy& numerics;
    const TaskContext& tasks;
};

enum class CallbackResult : std::uint8_t {
    ok,
    error,
};

using FrameCallback = CallbackResult (*)(void*, const CallbackContext&);

struct CallbackRegistration {
    // Runtime copies name. The host retains user_data ownership and must keep
    // it valid until no future step can invoke this callback.
    std::string_view name;
    FrameCallback callback = nullptr;
    void* user_data = nullptr;
};

struct StepResult {
    std::size_t callbacks_executed = 0;
    std::uint64_t start_ns = 0;
    std::uint64_t finish_ns = 0;
    bool deadline_missed = false;
};

struct ExecutorStats {
    ExecutorPolicy policy = ExecutorPolicy::static_deterministic;
    std::size_t worker_count = 0;
    std::size_t queue_capacity = 0;
    std::uint64_t submitted_tasks = 0;
    std::uint64_t local_executions = 0;
    std::uint64_t steal_attempts = 0;
    std::uint64_t successful_steals = 0;
    std::uint64_t queue_full_rejections = 0;
    std::uint64_t scratch_exhaustions = 0;
    std::uint64_t worker_starts = 0;
};

struct StaticPhaseAssignment {
    PhaseHandle phase{};
    std::size_t worker_index = 0;
};

struct MemoryPlan {
    // planned_bytes is the sum of both control fields and the three
    // *_total/storage fields. It describes requested runtime storage, not RSS.
    std::size_t memory_budget_bytes = 0;
    std::size_t planned_bytes = 0;
    std::size_t runtime_control_bytes = 0;
    std::size_t executor_control_bytes = 0;
    std::size_t phase_count = 0;
    std::size_t phase_scratch_bytes = 0;
    std::size_t phase_scratch_stride = 0;
    std::size_t phase_scratch_total_bytes = 0;
    std::size_t task_scratch_bytes = 0;
    std::size_t task_scratch_stride = 0;
    std::size_t task_scratch_slots = 0;
    std::size_t task_scratch_total_bytes = 0;
    std::size_t trace_capacity = 0;
    std::size_t trace_storage_bytes = 0;
    std::size_t queue_slots = 0;
    std::size_t scratch_alignment = 0;
    OverloadPolicy overload_policy = OverloadPolicy::reject_submission;
};

enum class RuntimeTraceEventType : std::uint8_t {
    finalized,
    started,
    step_begin,
    callback_begin,
    callback_end,
    step_end,
    stopped,
};

struct RuntimeTraceEvent {
    RuntimeTraceEventType type = RuntimeTraceEventType::step_begin;
    Status status = Status::ok;
    std::uint64_t timestamp_ns = 0;
    std::uint64_t frame_index = 0;
    std::size_t callback_index = 0;
};

// Host-driven lifecycle introduced by M1. Control methods are single-host-
// thread operations. A step invokes callbacks synchronously and never paces or
// sleeps; self-paced execution is a separate M5 concern.
class Runtime {
public:
    Runtime();
    explicit Runtime(RuntimeClock& clock);
    ~Runtime();

    Runtime(Runtime&&) noexcept;
    Runtime& operator=(Runtime&&) noexcept;

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    [[nodiscard]] Status configure(const RuntimeConfig& config) noexcept;
    [[nodiscard]] Status configure_key(
        std::string_view key,
        std::string_view value) noexcept;
    [[nodiscard]] Status register_callback(
        const CallbackRegistration& registration) noexcept;
    // The returned phase handle is required when defining dependencies or
    // logical resource access. Handles are valid only for this Runtime.
    [[nodiscard]] Status register_callback(
        const CallbackRegistration& registration,
        PhaseHandle& out_phase) noexcept;
    [[nodiscard]] Status register_resource(
        std::string_view name,
        ResourceHandle& out_resource) noexcept;
    [[nodiscard]] Status add_dependency(
        PhaseHandle prerequisite,
        PhaseHandle dependent) noexcept;
    [[nodiscard]] Status declare_resource_access(
        PhaseHandle phase,
        ResourceHandle resource,
        ResourceAccess access) noexcept;
    [[nodiscard]] Status finalize() noexcept;
    [[nodiscard]] Status start() noexcept;
    [[nodiscard]] Status step(
        const HostFrameContext& frame,
        StepResult* result = nullptr) noexcept;
    [[nodiscard]] Status stop() noexcept;

    [[nodiscard]] RuntimeState state() const noexcept;
    [[nodiscard]] const RuntimeConfig& config() const noexcept;
    [[nodiscard]] std::size_t callback_count() const noexcept;
    [[nodiscard]] std::size_t resource_count() const noexcept;
    [[nodiscard]] std::size_t dependency_count() const noexcept;
    [[nodiscard]] std::size_t resource_access_count() const noexcept;
    // Available after successful finalization. The order is deterministic and
    // remains stable through running and stopped states.
    [[nodiscard]] bool compiled_phase_at(
        std::size_t execution_index,
        PhaseHandle& phase) const noexcept;
    [[nodiscard]] bool static_phase_assignment_at(
        std::size_t registration_index,
        StaticPhaseAssignment& assignment) const noexcept;
    [[nodiscard]] ExecutorStats executor_stats() const noexcept;
    // Available after successful finalization. Counts describe requested
    // runtime payload/control storage and exclude allocator metadata and OS
    // thread stacks.
    [[nodiscard]] bool memory_plan(MemoryPlan& plan) const noexcept;
    [[nodiscard]] std::uint64_t now_ns() noexcept;
    // The view remains valid until the next control operation or destruction.
    [[nodiscard]] std::string_view last_error() const noexcept;

    [[nodiscard]] std::size_t trace_event_count() const noexcept;
    [[nodiscard]] bool trace_event(
        std::size_t chronological_index,
        RuntimeTraceEvent& event) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct DemoPipeline {
    struct State {
        std::atomic<std::uint64_t> ingestCount{0};
        std::atomic<std::uint64_t> gpuCount{0};
        std::atomic<std::uint64_t> ioCount{0};
        std::atomic<std::uint64_t> composeCount{0};
        std::atomic<std::uint64_t> fenceWaits{0};
        std::array<std::atomic<std::uint64_t>, 4> rungEventsSeen{};
    };

    DemoPipeline() = default;
    explicit DemoPipeline(std::shared_ptr<State> state) : state_(std::move(state)) {}

    [[nodiscard]] bool valid() const { return static_cast<bool>(state_); }
    [[nodiscard]] std::uint64_t ingest_frames() const;
    [[nodiscard]] std::uint64_t gpu_frames() const;
    [[nodiscard]] std::uint64_t io_frames() const;
    [[nodiscard]] std::uint64_t compose_frames() const;
    [[nodiscard]] std::uint64_t fence_waits() const;

private:
    std::shared_ptr<State> state_{};

    [[nodiscard]] std::uint64_t loadCounter(const std::atomic<std::uint64_t>& counter) const;

    friend DemoPipeline build_demo_pipeline(SimCore& sim);
};

DemoPipeline build_demo_pipeline(SimCore& sim);

inline std::uint64_t DemoPipeline::loadCounter(const std::atomic<std::uint64_t>& counter) const {
    return counter.load(std::memory_order_acquire);
}

inline std::uint64_t DemoPipeline::ingest_frames() const {
    return state_ ? loadCounter(state_->ingestCount) : 0;
}

inline std::uint64_t DemoPipeline::gpu_frames() const {
    return state_ ? loadCounter(state_->gpuCount) : 0;
}

inline std::uint64_t DemoPipeline::io_frames() const {
    return state_ ? loadCounter(state_->ioCount) : 0;
}

inline std::uint64_t DemoPipeline::compose_frames() const {
    return state_ ? loadCounter(state_->composeCount) : 0;
}

inline std::uint64_t DemoPipeline::fence_waits() const {
    return state_ ? loadCounter(state_->fenceWaits) : 0;
}

} // namespace rt
