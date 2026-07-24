#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
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
    bool self_paced_time;
    bool frame_watchdog;
    bool strict_platform_preflight;
    bool versioned_observability;
};

// Query runtime capabilities
Capabilities query_capabilities() noexcept;

// Example function using strong types
core::seconds tick_duration(core::seconds dt) noexcept;

inline constexpr std::uint32_t runtime_config_schema_version = 5;
inline constexpr std::uint32_t observability_schema_version = 1;
inline constexpr std::size_t observability_identifier_capacity = 64;
inline constexpr std::uint32_t observability_metadata_size = 184;

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
    platform_preflight_failed = -13,
    clock_failure = -14,
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

enum class PlatformPreflightMode : std::uint8_t {
    disabled,
    strict,
};

enum class PlatformCheckId : std::uint8_t {
    absolute_monotonic_clock,
    realtime_kernel,
    memory_lock_limit,
    locked_memory,
    isolated_cpu_affinity,
    realtime_scheduler,
};

enum class PlatformCheckStatus : std::uint8_t {
    passed,
    failed,
    unsupported,
};

inline constexpr std::size_t platform_check_capacity = 6;
inline constexpr std::size_t platform_check_message_capacity = 96;

struct PlatformCheckResult {
    PlatformCheckId id = PlatformCheckId::absolute_monotonic_clock;
    PlatformCheckStatus status = PlatformCheckStatus::unsupported;
    std::int32_t system_error = 0;
    std::array<char, platform_check_message_capacity> message{};
};

struct PlatformPreflightReport {
    PlatformPreflightMode mode = PlatformPreflightMode::disabled;
    bool passed = false;
    std::size_t check_count = 0;
    std::array<PlatformCheckResult, platform_check_capacity> checks{};
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
    std::uint64_t watchdog_timeout_ns = 0;
    std::uint32_t watchdog_max_degradation_level = 0;
    PlatformPreflightMode platform_preflight_mode =
        PlatformPreflightMode::disabled;
    // Stable caller-supplied provenance copied into every observability
    // snapshot. IDs use [A-Za-z0-9._:/@-] and must be NUL terminated.
    std::array<char, observability_identifier_capacity> workload_id{
        'u', 'n', 's', 'p', 'e', 'c', 'i', 'f', 'i', 'e', 'd', '\0'};
};

// Applies one strict schema key to a typed configuration. The supported keys
// are callback_capacity, scratch_bytes, trace_capacity, numerical_mode,
// executor_policy, worker_count, executor_queue_capacity, scratch_alignment,
// task_scratch_bytes, task_scratch_slots, memory_budget_bytes,
// overload_policy, watchdog_timeout_ns, watchdog_max_degradation_level,
// platform_preflight_mode, and workload_id. Unknown keys and partially parsed
// values are rejected.
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
    // Waits until an absolute timestamp in the same clock domain. Custom
    // clocks that do not implement absolute waiting keep host-driven stepping
    // but periodic execution returns clock_failure.
    [[nodiscard]] virtual Status sleep_until_ns(
        std::uint64_t absolute_ns) noexcept {
        (void)absolute_ns;
        return Status::clock_failure;
    }
    [[nodiscard]] virtual bool supports_absolute_sleep() const noexcept {
        return false;
    }
};

class PlatformPreflightProbe {
public:
    virtual ~PlatformPreflightProbe() = default;
    // Implementations must overwrite report without allocating or throwing.
    // A Runtime constructed with a probe borrows it for the Runtime lifetime.
    virtual void inspect(
        std::size_t planned_runtime_bytes,
        const RuntimeClock& clock,
        PlatformPreflightReport& report) noexcept = 0;
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
    // Runtime-owned degradation state. A watchdog event is applied only after
    // its frame returns, so callbacks observe the level committed by earlier
    // frames.
    std::uint32_t degradation_level = 0;
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
    bool watchdog_fired = false;
    std::uint32_t degradation_level = 0;
};

struct PeriodicRunConfig {
    std::uint64_t first_frame_index = 0;
    std::size_t frame_count = 1;
    std::chrono::nanoseconds period{16'666'667};
    std::optional<std::uint64_t> first_release_ns{};
    std::chrono::nanoseconds relative_deadline{16'666'667};
};

struct PeriodicFrameResult {
    Status status = Status::ok;
    std::uint64_t frame_index = 0;
    std::uint64_t release_ns = 0;
    std::uint64_t wake_ns = 0;
    std::uint64_t start_ns = 0;
    std::uint64_t finish_ns = 0;
    std::int64_t slack_ns = 0;
    bool deadline_missed = false;
    bool watchdog_fired = false;
    std::uint32_t degradation_level = 0;
};

using PeriodicFrameObserver = CallbackResult (*)(
    void* user_data,
    const PeriodicFrameResult& frame);

struct PeriodicRunResult {
    std::size_t frames_executed = 0;
    std::size_t deadline_misses = 0;
    std::size_t watchdog_events = 0;
    std::uint32_t final_degradation_level = 0;
    std::uint64_t first_release_ns = 0;
    std::uint64_t next_release_ns = 0;
    PeriodicFrameResult last_frame{};
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
    std::size_t trace_slot_bytes = 0;
    std::size_t trace_storage_bytes = 0;
    std::size_t queue_slots = 0;
    std::size_t scratch_alignment = 0;
    OverloadPolicy overload_policy = OverloadPolicy::reject_submission;
};

enum class RuntimeTraceEventType : std::uint16_t {
    finalized = 1,
    started = 2,
    periodic_release = 3,
    periodic_wake = 4,
    step_begin = 5,
    callback_begin = 6,
    callback_end = 7,
    watchdog_fired = 8,
    degradation_applied = 9,
    step_end = 10,
    stopped = 11,
};

enum class RuntimeTraceProducer : std::uint16_t {
    host = 0,
    worker = 1,
};

struct RuntimeTraceEvent {
    std::uint32_t schema_version = observability_schema_version;
    std::uint16_t record_size = 64;
    RuntimeTraceEventType type = RuntimeTraceEventType::step_begin;
    Status status = Status::ok;
    RuntimeTraceProducer producer = RuntimeTraceProducer::host;
    std::uint16_t reserved0 = 0;
    std::uint64_t sequence = 0;
    std::uint64_t timestamp_ns = 0;
    std::uint64_t frame_index = 0;
    std::uint32_t callback_index =
        std::numeric_limits<std::uint32_t>::max();
    std::uint32_t worker_index =
        std::numeric_limits<std::uint32_t>::max();
    // Event-specific fixed-width value. Finalization carries config_id,
    // callback events carry task_index, periodic release/wake carry their
    // absolute release, watchdog carries its timeout, and degradation carries
    // the applied level.
    std::uint64_t value = 0;
    std::uint64_t reserved1 = 0;
};

static_assert(sizeof(RuntimeTraceEvent) == 64);

enum class RuntimeMetricKind : std::uint8_t {
    counter = 0,
    gauge = 1,
};

enum class RuntimeMetricWindow : std::uint8_t {
    cumulative = 0,
    interval = 1,
};

// Numeric values are schema identifiers and remain stable for observability
// schema version 1.
enum class RuntimeMetricId : std::uint16_t {
    frames_started = 0,
    frames_completed = 1,
    frames_failed = 2,
    callbacks_started = 3,
    callbacks_completed = 4,
    callback_failures = 5,
    deadline_misses = 6,
    watchdog_events = 7,
    degradation_events = 8,
    periodic_releases = 9,
    periodic_wakes = 10,
    trace_events_emitted = 11,
    trace_events_overwritten = 12,
    trace_events_dropped = 13,
    executor_submitted_tasks = 14,
    executor_local_executions = 15,
    executor_steal_attempts = 16,
    executor_successful_steals = 17,
    executor_queue_rejections = 18,
    executor_scratch_exhaustions = 19,
    executor_worker_starts = 20,
    degradation_level = 21,
    count = 22,
};

inline constexpr std::size_t runtime_metric_count =
    static_cast<std::size_t>(RuntimeMetricId::count);

struct RuntimeMetricDefinition {
    RuntimeMetricId id = RuntimeMetricId::frames_started;
    RuntimeMetricKind kind = RuntimeMetricKind::counter;
    std::string_view name{};
};

[[nodiscard]] bool runtime_metric_definition(
    std::size_t schema_index,
    RuntimeMetricDefinition& definition) noexcept;
[[nodiscard]] const char* runtime_trace_event_name(
    RuntimeTraceEventType type) noexcept;

struct ObservabilityMetadata {
    std::uint32_t struct_size = observability_metadata_size;
    std::uint32_t schema_version = observability_schema_version;
    std::uint32_t runtime_version_major = version_major;
    std::uint32_t runtime_version_minor = version_minor;
    std::uint32_t runtime_version_patch = version_patch;
    std::uint32_t trace_event_size =
        static_cast<std::uint32_t>(sizeof(RuntimeTraceEvent));
    std::uint32_t metric_sample_size = 16;
    std::uint32_t metric_count = runtime_metric_count;
    std::uint64_t config_id = 0;
    std::uint64_t runtime_id = 0;
    std::uint64_t trace_capacity = 0;
    std::array<char, observability_identifier_capacity> build_id{};
    std::array<char, observability_identifier_capacity> workload_id{};
};

static_assert(sizeof(ObservabilityMetadata) == observability_metadata_size);

struct RuntimeMetricSample {
    RuntimeMetricId id = RuntimeMetricId::frames_started;
    RuntimeMetricKind kind = RuntimeMetricKind::counter;
    std::uint8_t reserved0 = 0;
    std::uint32_t reserved1 = 0;
    std::uint64_t value = 0;
};

static_assert(sizeof(RuntimeMetricSample) == 16);

struct RuntimeMetricCursor {
    std::uint32_t schema_version = observability_schema_version;
    std::uint32_t reserved0 = 0;
    std::uint64_t runtime_id = 0;
    std::uint64_t window_end_ns = 0;
    std::array<std::uint64_t, runtime_metric_count> counters{};
};

struct RuntimeMetricSnapshot {
    ObservabilityMetadata metadata{};
    RuntimeMetricWindow window = RuntimeMetricWindow::cumulative;
    std::array<std::uint8_t, 7> reserved0{};
    std::uint64_t snapshot_sequence = 0;
    std::uint64_t window_start_ns = 0;
    std::uint64_t window_end_ns = 0;
    std::size_t sample_count = 0;
    std::array<RuntimeMetricSample, runtime_metric_count> samples{};
};

struct RuntimeTraceCursor {
    std::uint32_t schema_version = observability_schema_version;
    std::uint32_t reserved0 = 0;
    std::uint64_t runtime_id = 0;
    std::uint64_t next_sequence = 0;
};

struct RuntimeTraceReadResult {
    ObservabilityMetadata metadata{};
    std::uint64_t first_sequence = 0;
    std::uint64_t next_sequence = 0;
    std::size_t events_read = 0;
    std::uint64_t lost_events = 0;
    std::uint64_t remaining_sequence_count = 0;
};

// Host-driven lifecycle introduced by M1. Control methods are single-host-
// thread operations. A step invokes callbacks synchronously and never paces or
// sleeps; self-paced execution uses the separate M5 run_periodic API.
class Runtime {
public:
    Runtime();
    explicit Runtime(RuntimeClock& clock);
    Runtime(RuntimeClock& clock, PlatformPreflightProbe& preflight);
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
    // Runs a finite runtime-owned loop on the calling frame thread. Releases
    // are first_release + i * period; late frames never shift that epoch.
    [[nodiscard]] Status run_periodic(
        const PeriodicRunConfig& config,
        PeriodicFrameObserver observer = nullptr,
        void* observer_data = nullptr,
        PeriodicRunResult* result = nullptr) noexcept;
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
    // Available after start is attempted. Strict failures leave the runtime
    // finalized so the host can inspect every failed prerequisite.
    [[nodiscard]] bool platform_preflight_report(
        PlatformPreflightReport& report) const noexcept;
    [[nodiscard]] std::uint32_t degradation_level() const noexcept;
    [[nodiscard]] std::uint64_t now_ns() noexcept;
    // The view remains valid until the next control operation or destruction.
    [[nodiscard]] std::string_view last_error() const noexcept;

    // Observability export is a non-RT host operation. These calls reject an
    // active step/periodic loop and never invoke host callbacks.
    [[nodiscard]] Status observability_metadata(
        ObservabilityMetadata& metadata) noexcept;
    // Interval windows use a caller-owned cursor. The first interval covers
    // finalization through this call; later intervals partition cumulative
    // counters without mutating runtime-global baselines. Gauges are sampled
    // at the window end and are never differenced. Fresh cursors must be
    // default initialized.
    [[nodiscard]] Status metrics_snapshot(
        RuntimeMetricWindow window,
        RuntimeMetricCursor* cursor,
        RuntimeMetricSnapshot& snapshot) noexcept;
    // A zeroed/default cursor starts at the oldest retained event. If an
    // established cursor falls behind, lost_events reports the exact skipped
    // sequence span. The caller owns output storage; fresh cursors must be
    // default initialized.
    [[nodiscard]] Status read_trace(
        RuntimeTraceCursor& cursor,
        std::span<RuntimeTraceEvent> output,
        RuntimeTraceReadResult& result) noexcept;

    // Compatibility accessors over the latest retained trace window.
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
