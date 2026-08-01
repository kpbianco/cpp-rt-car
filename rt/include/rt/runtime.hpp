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
#include "rt/config.hpp"
#include "rt/device.hpp"
#include "rt/graph.hpp"
#include "rt/status.hpp"
#include "rt/version.hpp"

class SimCore;

namespace rt {

// Capabilities report only completed target-path guarantees. A false value
// identifies a later roadmap milestone rather than a disabled build option.
struct Capabilities {
    bool compiled_graph;
    bool host_driven_time;
    bool unified_cpu_executor;
    bool host_executor_adapter;
    bool bounded_memory_plan;
    bool self_paced_time;
    bool frame_watchdog;
    bool strict_platform_preflight;
    bool versioned_observability;
    bool deterministic_replay;
    bool bounded_device_backend;
};

// Query runtime capabilities
Capabilities query_capabilities() noexcept;

// Deprecated 1.x compatibility shim. New code should use the typed duration
// fields on HostFrameContext and PeriodicRunConfig directly.
[[deprecated("use the runtime frame/period duration fields directly")]]
core::seconds tick_duration(core::seconds dt) noexcept;

inline constexpr std::uint32_t observability_schema_version = 2;
inline constexpr std::uint32_t observability_metadata_size = 184;
inline constexpr std::uint32_t checkpoint_schema_version = 1;
inline constexpr std::uint32_t input_log_schema_version = 1;
inline constexpr std::size_t replay_identifier_capacity =
    observability_identifier_capacity;

enum class RuntimeState : std::uint8_t {
    configuring,
    finalized,
    running,
    stopped,
};

/*
 * One immutable job record submitted to a borrowed engine job system. The
 * adapter copies this record before submit() returns and invokes execute
 * exactly once for every accepted job. Scratch is runtime-owned, exclusive to
 * the invocation, and valid until execute returns. completion_context is an
 * opaque runtime token and must only be passed back to execute.
 */
using HostJobExecute = void (*)(
    void* execution_context,
    void* completion_context,
    std::uint64_t completion_token,
    std::uint32_t worker_index);

struct HostExecutorJob {
    HostJobExecute execute = nullptr;
    void* execution_context = nullptr;
    void* completion_context = nullptr;
    std::uint64_t completion_token = 0;
    std::byte* scratch = nullptr;
    std::size_t scratch_bytes = 0;
};

using HostExecutorSubmit = Status (*)(
    void* user_data,
    const HostExecutorJob& job) noexcept;
using HostExecutorTryExecuteOne = bool (*)(void* user_data) noexcept;

/*
 * A borrowed, already-running host job system. The declared worker/queue
 * capacities must exactly match RuntimeConfig when host_adapter is selected.
 * submit() is bounded and returns queue_full without accepting the job when
 * capacity is unavailable. try_execute_one() may execute at most one queued
 * job and is required so nested runtime work cannot deadlock a saturated host
 * team. The adapter and user_data outlive Runtime::stop().
 */
struct HostExecutorAdapter {
    void* user_data = nullptr;
    std::size_t worker_count = 0;
    std::size_t queue_capacity = 0;
    HostExecutorSubmit submit = nullptr;
    HostExecutorTryExecuteOne try_execute_one = nullptr;
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

struct ThreadPolicyProviderCapabilities {
    bool cpu_affinity = false;
    bool scheduling = false;
    bool thread_name = false;
    // Includes the terminating NUL. Zero means names are unsupported.
    std::size_t thread_name_capacity = 0;
};

// M15-02 providers operate only on the calling thread. Runtime-owned lanes
// invoke these methods before leaving their startup barrier; the frame lane
// is inspected but never mutated. A Runtime borrows an injected provider
// through stop/destruction. Capabilities must remain immutable after the
// provider is attached, and provider methods must not allocate or throw.
class ThreadPolicyProvider {
public:
    virtual ~ThreadPolicyProvider() = default;
    [[nodiscard]] virtual ThreadPolicyProviderCapabilities capabilities()
        const noexcept = 0;
    [[nodiscard]] virtual Status apply_current_thread(
        ThreadResourceId id,
        const ThreadPolicy& policy,
        ThreadPolicy& applied,
        int& system_error) noexcept = 0;
    [[nodiscard]] virtual Status inspect_current_thread(
        ThreadResourceId id,
        ThreadPolicy& observed,
        int& system_error) noexcept = 0;
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

enum class CallbackResult : std::uint8_t {
    ok,
    error,
};

struct StateRegistration {
    // State names are stable schema identifiers and use the same restricted
    // character set as workload_id. Storage is borrowed through Runtime
    // destruction. Its bytes must be an application-defined canonical
    // representation (for example, fixed-width little-endian fields), not a
    // compiler-native object layout with padding.
    std::string_view name;
    std::uint32_t schema_version = 1;
    std::span<std::byte> storage{};
};

struct ArtifactWriteResult {
    std::size_t required_bytes = 0;
    std::size_t bytes_written = 0;
    std::uint64_t checksum = 0;
};

struct CheckpointMetadata {
    std::uint32_t schema_version = checkpoint_schema_version;
    std::uint32_t runtime_version_major = version_major;
    std::uint32_t runtime_version_minor = version_minor;
    std::uint32_t runtime_version_patch = version_patch;
    DeterminismTier determinism_tier = DeterminismTier::unspecified;
    std::uint64_t config_id = 0;
    std::uint64_t replay_id = 0;
    std::uint64_t graph_id = 0;
    std::uint64_t state_schema_id = 0;
    std::uint64_t checkpoint_frame_index = 0;
    std::uint64_t state_payload_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t state_hash = 0;
    std::uint64_t artifact_checksum = 0;
    std::uint32_t state_count = 0;
    std::array<char, replay_identifier_capacity> build_id{};
    std::array<char, replay_identifier_capacity> workload_id{};
};

struct ReplayInputRecord {
    HostFrameContext frame{};
    std::uint32_t input_type = 0;
    std::span<const std::byte> payload{};
};

struct InputLogMetadata {
    std::uint32_t schema_version = input_log_schema_version;
    std::uint32_t runtime_version_major = version_major;
    std::uint32_t runtime_version_minor = version_minor;
    std::uint32_t runtime_version_patch = version_patch;
    DeterminismTier determinism_tier = DeterminismTier::unspecified;
    std::uint64_t replay_id = 0;
    std::uint64_t state_schema_id = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t total_bytes = 0;
    std::uint64_t artifact_checksum = 0;
    std::uint64_t first_frame_index = 0;
    std::uint64_t last_frame_index = 0;
    std::uint32_t record_count = 0;
    std::array<char, replay_identifier_capacity> workload_id{};
};

struct ReplayInputView {
    HostFrameContext frame{};
    std::uint32_t input_type = 0;
    std::span<const std::byte> payload{};
};

using ReplayInputCallback = CallbackResult (*)(
    void* user_data,
    const ReplayInputView& input);

struct ReplayResult {
    std::uint64_t checkpoint_frame_index = 0;
    std::uint64_t first_frame_index = 0;
    std::uint64_t last_frame_index = 0;
    std::size_t records_processed = 0;
    std::size_t frames_replayed = 0;
    std::uint64_t final_state_hash = 0;
};

// These inspectors are allocation-free and validate the complete artifact,
// including fixed-width little-endian headers, bounds, reserved fields, and
// per-record plus whole-artifact checksums. They do not apply runtime identity
// policy; Runtime::restore_checkpoint and Runtime::replay do.
[[nodiscard]] Status inspect_checkpoint_artifact(
    std::span<const std::byte> artifact,
    CheckpointMetadata& metadata) noexcept;
[[nodiscard]] Status inspect_input_log_artifact(
    std::span<const std::byte> artifact,
    InputLogMetadata& metadata) noexcept;

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

using FrameCallback = CallbackResult (*)(void*, const CallbackContext&);

struct CallbackRegistration {
    // Runtime copies name. The host retains user_data ownership and must keep
    // it valid until no future step can invoke this callback.
    std::string_view name;
    FrameCallback callback = nullptr;
    void* user_data = nullptr;
};

struct DeviceCallbackContext {
    const HostFrameContext& frame;
    std::span<std::byte> scratch;
    const NumericalPolicy& numerics;
    const TaskContext& tasks;
    std::uint32_t degradation_level = 0;
};

using DeviceCommandCallback = CallbackResult (*)(
    void* user_data,
    const DeviceCallbackContext& context,
    DeviceSubmission& submission);

struct DevicePhaseRegistration {
    // Runtime copies name and borrows user_data until no future step can
    // invoke the command provider. The provider prepares one fixed-size
    // submission and must return without waiting for device completion.
    std::string_view name;
    DeviceBackendHandle backend{};
    DeviceCommandCallback callback = nullptr;
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

enum class ThreadOwnership : std::uint8_t {
    runtime,
    host,
    backend,
};

enum class PolicyStageState : std::uint8_t {
    not_requested,
    portable_default,
    portable_fallback,
    native_resolved,
    not_performed,
    verify_only,
    applied,
    verified,
};

enum class MemoryAccountingScope : std::uint8_t {
    runtime_plan,
    informational_excluded,
};

struct ThreadPolicyReport {
    ThreadResourceId id{};
    std::uint64_t accounting_key = 0;
    ThreadOwnership ownership = ThreadOwnership::runtime;
    bool explicitly_requested = false;
    ThreadPolicy requested{};
    ThreadPolicy resolved{};
    ThreadPolicy applied{};
    ThreadPolicy verified{};
    PolicyStageState resolution = PolicyStageState::not_requested;
    PolicyStageState application = PolicyStageState::not_performed;
    PolicyStageState verification = PolicyStageState::not_performed;
    Status application_status = Status::ok;
    Status verification_status = Status::ok;
    int application_system_error = 0;
    int verification_system_error = 0;
    bool rolled_back = false;
};

struct MemoryRegionPolicyReport {
    MemoryRegionId id{};
    std::uint64_t accounting_key = 0;
    MemoryProviderOwnership ownership = MemoryProviderOwnership::runtime;
    MemoryAccountingScope accounting_scope =
        MemoryAccountingScope::runtime_plan;
    bool explicitly_requested = false;
    std::size_t reported_bytes = 0;
    std::size_t accounted_bytes = 0;
    std::size_t requested_footprint_bytes = 0;
    MemoryRegionPolicy requested{};
    MemoryRegionPolicy resolved{};
    MemoryRegionPolicy applied{};
    PolicyStageState resolution = PolicyStageState::not_requested;
    PolicyStageState application = PolicyStageState::not_performed;
    PolicyStageState verification = PolicyStageState::not_performed;
};

struct CpuMemoryPolicySummary {
    std::size_t thread_count = 0;
    std::size_t memory_region_count = 0;
    std::size_t runtime_owned_thread_count = 0;
    std::size_t externally_owned_thread_count = 0;
    std::size_t runtime_accounted_bytes = 0;
    std::size_t informational_excluded_bytes = 0;
};

struct MemoryPlan {
    // planned_bytes is the sum of the three control fields and the three
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
    std::size_t state_count = 0;
    // Borrowed application storage is reported but is not included in
    // planned_bytes. Registry metadata is included in runtime_control_bytes.
    std::size_t registered_state_bytes = 0;
    std::size_t snapshot_max_bytes = 0;
    std::size_t replay_input_capacity = 0;
    std::size_t input_log_max_bytes = 0;
    std::size_t device_backend_count = 0;
    std::size_t device_buffer_count = 0;
    std::size_t device_outstanding_capacity = 0;
    std::size_t device_completion_batch = 0;
    std::size_t device_control_bytes = 0;
    // Backend-reported private control storage is informational and excluded
    // from planned_bytes because the backend owns it.
    std::size_t device_backend_reported_bytes = 0;
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
    device_submitted = 12,
    device_completed = 13,
    device_reset = 14,
};

enum class RuntimeTraceProducer : std::uint16_t {
    host = 0,
    worker = 1,
    device_service = 2,
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
// schema version 2. IDs 0-21 retain their schema version 1 meanings.
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
    device_submissions = 22,
    device_completions = 23,
    device_failures = 24,
    device_queue_rejections = 25,
    device_timeouts = 26,
    device_losses = 27,
    device_resets = 28,
    device_service_polls = 29,
    device_outstanding = 30,
    device_service_starts = 31,
    count = 32,
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
    // Copies every request. Validation and portable resolution occur
    // transactionally during finalize(); schema-v7 and C ABI v8 are unchanged.
    [[nodiscard]] Status set_cpu_memory_policy(
        const CpuMemoryPolicyRequest& policy) noexcept;
    // Replaces the default platform provider for deterministic integration
    // tests or host-specific policy control. May be called only while
    // configuring; the provider is borrowed through stop/destruction.
    [[nodiscard]] Status set_thread_policy_provider(
        ThreadPolicyProvider& provider) noexcept;
    // Copies the callback table and borrows adapter.user_data through stop().
    // May be called only while configuring.
    [[nodiscard]] Status set_host_executor(
        const HostExecutorAdapter& adapter) noexcept;
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
    [[nodiscard]] Status register_device_backend(
        const DeviceBackendRegistration& registration,
        DeviceBackendHandle& out_backend) noexcept;
    [[nodiscard]] Status register_device_buffer(
        const DeviceBufferRegistration& registration,
        DeviceBufferHandle& out_buffer) noexcept;
    [[nodiscard]] Status register_device_phase(
        const DevicePhaseRegistration& registration,
        PhaseHandle& out_phase) noexcept;
    [[nodiscard]] Status register_resource(
        std::string_view name,
        ResourceHandle& out_resource) noexcept;
    [[nodiscard]] Status register_state(
        const StateRegistration& registration) noexcept;
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
    // Device cleanup failures retain borrowed ownership, quiesce execution,
    // and leave the public lifecycle state unchanged. Retry stop() until it
    // succeeds before releasing borrowed resources. The destructor cannot
    // report this status and is only a best-effort fallback.
    [[nodiscard]] Status stop() noexcept;
    [[nodiscard]] Status device_health(
        DeviceBackendHandle backend,
        DeviceHealth& health) noexcept;
    [[nodiscard]] Status reset_device(
        DeviceBackendHandle backend) noexcept;

    [[nodiscard]] RuntimeState state() const noexcept;
    [[nodiscard]] const RuntimeConfig& config() const noexcept;
    [[nodiscard]] std::size_t callback_count() const noexcept;
    [[nodiscard]] std::size_t device_backend_count() const noexcept;
    [[nodiscard]] std::size_t device_buffer_count() const noexcept;
    [[nodiscard]] std::size_t device_phase_count() const noexcept;
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
    // Available after successful finalization. Numeric role/category IDs and
    // accounting keys are stable within the M15 C++ source contract.
    [[nodiscard]] bool cpu_memory_policy_summary(
        CpuMemoryPolicySummary& summary) const noexcept;
    [[nodiscard]] bool thread_policy_report_at(
        std::size_t index,
        ThreadPolicyReport& report) const noexcept;
    [[nodiscard]] bool memory_policy_report_at(
        std::size_t index,
        MemoryRegionPolicyReport& report) const noexcept;
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

    // Checkpoint and replay calls are non-RT host operations. Buffers are
    // caller-owned, and their maximum accepted sizes are frozen by
    // RuntimeConfig. A failed restore never mutates registered state.
    [[nodiscard]] Status checkpoint_size(
        std::size_t& required_bytes) noexcept;
    [[nodiscard]] Status write_checkpoint(
        std::uint64_t checkpoint_frame_index,
        std::span<std::byte> output,
        ArtifactWriteResult& result) noexcept;
    [[nodiscard]] Status restore_checkpoint(
        std::span<const std::byte> checkpoint,
        CheckpointMetadata* metadata = nullptr) noexcept;
    [[nodiscard]] Status write_input_log(
        std::span<const ReplayInputRecord> records,
        std::span<std::byte> output,
        ArtifactWriteResult& result) noexcept;
    [[nodiscard]] Status replay(
        std::span<const std::byte> checkpoint,
        std::span<const std::byte> input_log,
        ReplayInputCallback input_callback,
        void* input_user_data = nullptr,
        ReplayResult* result = nullptr) noexcept;
    [[nodiscard]] Status registered_state_hash(
        std::uint64_t& hash) noexcept;

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

[[deprecated(
    "legacy SimCore demo; use rt::Runtime and the rtfw::runtime target")]]
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
