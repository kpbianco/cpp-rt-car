#include <rt/c_api.h>
#include <rt/runtime.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string_view>
#include <vector>

namespace {

static_assert(
    static_cast<std::uint32_t>(
        rt::PlatformCheckId::absolute_monotonic_clock) ==
    RTFW_PLATFORM_CHECK_ABSOLUTE_MONOTONIC_CLOCK);
static_assert(
    static_cast<std::uint32_t>(
        rt::PlatformCheckId::realtime_kernel) ==
    RTFW_PLATFORM_CHECK_REALTIME_KERNEL);
static_assert(
    static_cast<std::uint32_t>(
        rt::PlatformCheckId::memory_lock_limit) ==
    RTFW_PLATFORM_CHECK_MEMORY_LOCK_LIMIT);
static_assert(
    static_cast<std::uint32_t>(
        rt::PlatformCheckId::locked_memory) ==
    RTFW_PLATFORM_CHECK_LOCKED_MEMORY);
static_assert(
    static_cast<std::uint32_t>(
        rt::PlatformCheckId::isolated_cpu_affinity) ==
    RTFW_PLATFORM_CHECK_ISOLATED_CPU_AFFINITY);
static_assert(
    static_cast<std::uint32_t>(
        rt::PlatformCheckId::realtime_scheduler) ==
    RTFW_PLATFORM_CHECK_REALTIME_SCHEDULER);
static_assert(
    static_cast<std::uint32_t>(
        rt::PlatformCheckStatus::passed) ==
    RTFW_PLATFORM_CHECK_PASSED);
static_assert(
    static_cast<std::uint32_t>(
        rt::PlatformCheckStatus::failed) ==
    RTFW_PLATFORM_CHECK_FAILED);
static_assert(
    static_cast<std::uint32_t>(
        rt::PlatformCheckStatus::unsupported) ==
    RTFW_PLATFORM_CHECK_UNSUPPORTED);
static_assert(
    rt::observability_schema_version ==
    RTFW_OBSERVABILITY_SCHEMA_VERSION);
static_assert(
    rt::observability_identifier_capacity ==
    RTFW_OBSERVABILITY_IDENTIFIER_CAPACITY);
static_assert(
    rt::checkpoint_schema_version ==
    RTFW_CHECKPOINT_SCHEMA_VERSION);
static_assert(
    rt::input_log_schema_version ==
    RTFW_INPUT_LOG_SCHEMA_VERSION);
static_assert(
    rt::replay_identifier_capacity ==
    RTFW_REPLAY_IDENTIFIER_CAPACITY);
static_assert(
    static_cast<std::uint32_t>(
        rt::DeterminismTier::unspecified) ==
        RTFW_DETERMINISM_D0_UNSPECIFIED &&
    static_cast<std::uint32_t>(
        rt::DeterminismTier::schedule_independent) ==
        RTFW_DETERMINISM_D1_SCHEDULE_INDEPENDENT &&
    static_cast<std::uint32_t>(
        rt::DeterminismTier::reproducible_build) ==
        RTFW_DETERMINISM_D2_REPRODUCIBLE_BUILD &&
    static_cast<std::uint32_t>(
        rt::DeterminismTier::portable_deterministic) ==
        RTFW_DETERMINISM_D3_PORTABLE);

constexpr std::array<std::uint16_t, 14> kCppTraceIds{{
    static_cast<std::uint16_t>(
        rt::RuntimeTraceEventType::finalized),
    static_cast<std::uint16_t>(
        rt::RuntimeTraceEventType::started),
    static_cast<std::uint16_t>(
        rt::RuntimeTraceEventType::periodic_release),
    static_cast<std::uint16_t>(
        rt::RuntimeTraceEventType::periodic_wake),
    static_cast<std::uint16_t>(
        rt::RuntimeTraceEventType::step_begin),
    static_cast<std::uint16_t>(
        rt::RuntimeTraceEventType::callback_begin),
    static_cast<std::uint16_t>(
        rt::RuntimeTraceEventType::callback_end),
    static_cast<std::uint16_t>(
        rt::RuntimeTraceEventType::watchdog_fired),
    static_cast<std::uint16_t>(
        rt::RuntimeTraceEventType::degradation_applied),
    static_cast<std::uint16_t>(
        rt::RuntimeTraceEventType::step_end),
    static_cast<std::uint16_t>(
        rt::RuntimeTraceEventType::stopped),
    static_cast<std::uint16_t>(
        rt::RuntimeTraceEventType::device_submitted),
    static_cast<std::uint16_t>(
        rt::RuntimeTraceEventType::device_completed),
    static_cast<std::uint16_t>(
        rt::RuntimeTraceEventType::device_reset),
}};
constexpr std::array<std::uint16_t, 14> kCTraceIds{{
    RTFW_TRACE_FINALIZED,
    RTFW_TRACE_STARTED,
    RTFW_TRACE_PERIODIC_RELEASE,
    RTFW_TRACE_PERIODIC_WAKE,
    RTFW_TRACE_STEP_BEGIN,
    RTFW_TRACE_CALLBACK_BEGIN,
    RTFW_TRACE_CALLBACK_END,
    RTFW_TRACE_WATCHDOG_FIRED,
    RTFW_TRACE_DEGRADATION_APPLIED,
    RTFW_TRACE_STEP_END,
    RTFW_TRACE_STOPPED,
    RTFW_TRACE_DEVICE_SUBMITTED,
    RTFW_TRACE_DEVICE_COMPLETED,
    RTFW_TRACE_DEVICE_RESET,
}};
static_assert(kCppTraceIds == kCTraceIds);
static_assert(
    static_cast<std::uint16_t>(rt::RuntimeTraceProducer::host) ==
        RTFW_TRACE_PRODUCER_HOST &&
    static_cast<std::uint16_t>(rt::RuntimeTraceProducer::worker) ==
        RTFW_TRACE_PRODUCER_WORKER &&
    static_cast<std::uint16_t>(
        rt::RuntimeTraceProducer::device_service) ==
        RTFW_TRACE_PRODUCER_DEVICE_SERVICE);

constexpr std::array<std::uint16_t, RTFW_RUNTIME_METRIC_COUNT>
    kCppMetricIds{{
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::frames_started),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::frames_completed),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::frames_failed),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::callbacks_started),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::callbacks_completed),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::callback_failures),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::deadline_misses),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::watchdog_events),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::degradation_events),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::periodic_releases),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::periodic_wakes),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::trace_events_emitted),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::trace_events_overwritten),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::trace_events_dropped),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::executor_submitted_tasks),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::executor_local_executions),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::executor_steal_attempts),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::executor_successful_steals),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::executor_queue_rejections),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::executor_scratch_exhaustions),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::executor_worker_starts),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::degradation_level),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::device_submissions),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::device_completions),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::device_failures),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::device_queue_rejections),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::device_timeouts),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::device_losses),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::device_resets),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::device_service_polls),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::device_outstanding),
        static_cast<std::uint16_t>(
            rt::RuntimeMetricId::device_service_starts),
    }};
constexpr std::array<std::uint16_t, RTFW_RUNTIME_METRIC_COUNT>
    kCMetricIds{{
        RTFW_METRIC_FRAMES_STARTED,
        RTFW_METRIC_FRAMES_COMPLETED,
        RTFW_METRIC_FRAMES_FAILED,
        RTFW_METRIC_CALLBACKS_STARTED,
        RTFW_METRIC_CALLBACKS_COMPLETED,
        RTFW_METRIC_CALLBACK_FAILURES,
        RTFW_METRIC_DEADLINE_MISSES,
        RTFW_METRIC_WATCHDOG_EVENTS,
        RTFW_METRIC_DEGRADATION_EVENTS,
        RTFW_METRIC_PERIODIC_RELEASES,
        RTFW_METRIC_PERIODIC_WAKES,
        RTFW_METRIC_TRACE_EVENTS_EMITTED,
        RTFW_METRIC_TRACE_EVENTS_OVERWRITTEN,
        RTFW_METRIC_TRACE_EVENTS_DROPPED,
        RTFW_METRIC_EXECUTOR_SUBMITTED_TASKS,
        RTFW_METRIC_EXECUTOR_LOCAL_EXECUTIONS,
        RTFW_METRIC_EXECUTOR_STEAL_ATTEMPTS,
        RTFW_METRIC_EXECUTOR_SUCCESSFUL_STEALS,
        RTFW_METRIC_EXECUTOR_QUEUE_REJECTIONS,
        RTFW_METRIC_EXECUTOR_SCRATCH_EXHAUSTIONS,
        RTFW_METRIC_EXECUTOR_WORKER_STARTS,
        RTFW_METRIC_DEGRADATION_LEVEL,
        RTFW_METRIC_DEVICE_SUBMISSIONS,
        RTFW_METRIC_DEVICE_COMPLETIONS,
        RTFW_METRIC_DEVICE_FAILURES,
        RTFW_METRIC_DEVICE_QUEUE_REJECTIONS,
        RTFW_METRIC_DEVICE_TIMEOUTS,
        RTFW_METRIC_DEVICE_LOSSES,
        RTFW_METRIC_DEVICE_RESETS,
        RTFW_METRIC_DEVICE_SERVICE_POLLS,
        RTFW_METRIC_DEVICE_OUTSTANDING,
        RTFW_METRIC_DEVICE_SERVICE_STARTS,
    }};
static_assert(kCppMetricIds == kCMetricIds);
static_assert(
    rt::runtime_metric_count ==
    RTFW_RUNTIME_METRIC_COUNT);
static_assert(sizeof(rt::RuntimeTraceEvent) == sizeof(rtfw_trace_event));
static_assert(
    sizeof(rt::ObservabilityMetadata) ==
    sizeof(rtfw_observability_metadata));
static_assert(
    sizeof(rt::RuntimeMetricSample) ==
    sizeof(rtfw_metric_sample));

rtfw_status to_c_status(rt::Status status) noexcept {
    switch (status) {
    case rt::Status::ok:
        return RTFW_STATUS_OK;
    case rt::Status::invalid_argument:
        return RTFW_STATUS_INVALID_ARGUMENT;
    case rt::Status::invalid_state:
        return RTFW_STATUS_INVALID_STATE;
    case rt::Status::invalid_config:
        return RTFW_STATUS_INVALID_CONFIG;
    case rt::Status::capacity_exceeded:
        return RTFW_STATUS_CAPACITY_EXCEEDED;
    case rt::Status::callback_failed:
        return RTFW_STATUS_CALLBACK_FAILED;
    case rt::Status::resource_exhausted:
        return RTFW_STATUS_RESOURCE_EXHAUSTED;
    case rt::Status::internal_error:
        return RTFW_STATUS_INTERNAL_ERROR;
    case rt::Status::invalid_handle:
        return RTFW_STATUS_INVALID_HANDLE;
    case rt::Status::graph_cycle:
        return RTFW_STATUS_GRAPH_CYCLE;
    case rt::Status::resource_conflict:
        return RTFW_STATUS_RESOURCE_CONFLICT;
    case rt::Status::queue_full:
        return RTFW_STATUS_QUEUE_FULL;
    case rt::Status::scratch_exhausted:
        return RTFW_STATUS_SCRATCH_EXHAUSTED;
    case rt::Status::platform_preflight_failed:
        return RTFW_STATUS_PLATFORM_PREFLIGHT_FAILED;
    case rt::Status::clock_failure:
        return RTFW_STATUS_CLOCK_FAILURE;
    case rt::Status::invalid_artifact:
        return RTFW_STATUS_INVALID_ARTIFACT;
    case rt::Status::incompatible_artifact:
        return RTFW_STATUS_INCOMPATIBLE_ARTIFACT;
    case rt::Status::device_queue_full:
        return RTFW_STATUS_DEVICE_QUEUE_FULL;
    case rt::Status::device_timeout:
        return RTFW_STATUS_DEVICE_TIMEOUT;
    case rt::Status::device_error:
        return RTFW_STATUS_DEVICE_ERROR;
    case rt::Status::device_lost:
        return RTFW_STATUS_DEVICE_LOST;
    case rt::Status::device_canceled:
        return RTFW_STATUS_DEVICE_CANCELED;
    case rt::Status::device_reset_required:
        return RTFW_STATUS_DEVICE_RESET_REQUIRED;
    case rt::Status::incompatible_abi:
        return RTFW_STATUS_INCOMPATIBLE_ABI;
    }
    return RTFW_STATUS_INTERNAL_ERROR;
}

bool bytes_are_zero(const uint8_t* bytes, std::size_t size) noexcept {
    for (std::size_t index = 0; index < size; ++index) {
        if (bytes[index] != 0u) {
            return false;
        }
    }
    return true;
}

bool bounded_c_identifier(
    const char* value,
    std::size_t capacity,
    std::string_view& output) noexcept {
    output = {};
    if (!value || capacity == 0) {
        return false;
    }
    std::size_t length = 0;
    while (length < capacity && value[length] != '\0') {
        ++length;
    }
    if (length == 0 || length == capacity) {
        return false;
    }
    output = std::string_view(value, length);
    return true;
}

bool config_header_valid(const rtfw_config& config) noexcept {
    return config.struct_size >= sizeof(rtfw_config) &&
           config.abi_version == RTFW_C_ABI_VERSION &&
           bytes_are_zero(
               reinterpret_cast<const uint8_t*>(config.reserved),
               sizeof(config.reserved));
}

bool abi_info_header_valid(const rtfw_abi_info& info) noexcept {
    return info.struct_size >= sizeof(rtfw_abi_info) &&
           bytes_are_zero(
               reinterpret_cast<const uint8_t*>(info.reserved),
               sizeof(info.reserved));
}

bool host_executor_header_valid(
    const rtfw_host_executor& executor) noexcept {
    return executor.struct_size >= sizeof(rtfw_host_executor) &&
           executor.abi_version == RTFW_C_ABI_VERSION &&
           executor.worker_count <=
               std::numeric_limits<std::size_t>::max() &&
           executor.queue_capacity <=
               std::numeric_limits<std::size_t>::max() &&
           executor.submit != nullptr &&
           executor.try_execute_one != nullptr &&
           bytes_are_zero(
               reinterpret_cast<const uint8_t*>(executor.reserved),
               sizeof(executor.reserved));
}

bool frame_header_valid(const rtfw_frame_context& frame) noexcept {
    return frame.struct_size >= sizeof(rtfw_frame_context) &&
           frame.reserved0 == 0u &&
           bytes_are_zero(frame.reserved1, sizeof(frame.reserved1));
}

bool result_header_valid(const rtfw_step_result& result) noexcept {
    return result.struct_size >= sizeof(rtfw_step_result) &&
           result.reserved0 == 0u &&
           bytes_are_zero(result.reserved1, sizeof(result.reserved1));
}

bool memory_plan_header_valid(
    const rtfw_memory_plan& plan) noexcept {
    return plan.struct_size >= sizeof(rtfw_memory_plan) &&
           bytes_are_zero(
               reinterpret_cast<const uint8_t*>(plan.reserved),
               sizeof(plan.reserved));
}

bool device_health_header_valid(
    const rtfw_device_health& health) noexcept {
    return health.struct_size >= sizeof(health) &&
           health.reserved0 == 0u &&
           bytes_are_zero(
               reinterpret_cast<const uint8_t*>(health.reserved),
               sizeof(health.reserved));
}

bool periodic_config_header_valid(
    const rtfw_periodic_config& config) noexcept {
    return config.struct_size >= sizeof(rtfw_periodic_config) &&
           config.reserved0 == 0u &&
           config.has_first_release <= 1u &&
           bytes_are_zero(config.reserved1, sizeof(config.reserved1)) &&
           bytes_are_zero(
               reinterpret_cast<const uint8_t*>(config.reserved),
               sizeof(config.reserved));
}

bool periodic_result_header_valid(
    const rtfw_periodic_run_result& result) noexcept {
    return result.struct_size >= sizeof(rtfw_periodic_run_result) &&
           result.reserved0 == 0u &&
           result.reserved1 == 0u &&
           result.last_frame.struct_size >=
               sizeof(rtfw_periodic_frame_result) &&
           bytes_are_zero(
               result.last_frame.reserved0,
               sizeof(result.last_frame.reserved0)) &&
           bytes_are_zero(
               reinterpret_cast<const uint8_t*>(result.reserved),
               sizeof(result.reserved));
}

bool preflight_report_header_valid(
    const rtfw_platform_preflight_report& report) noexcept {
    return report.struct_size >=
               sizeof(rtfw_platform_preflight_report) &&
           bytes_are_zero(report.reserved0, sizeof(report.reserved0)) &&
           bytes_are_zero(
               reinterpret_cast<const uint8_t*>(report.reserved),
               sizeof(report.reserved));
}

bool observability_metadata_header_valid(
    const rtfw_observability_metadata& metadata) noexcept {
    return metadata.struct_size >=
        sizeof(rtfw_observability_metadata);
}

bool metric_cursor_header_valid(
    const rtfw_metric_cursor& cursor) noexcept {
    return cursor.struct_size >= sizeof(rtfw_metric_cursor) &&
           cursor.schema_version ==
               RTFW_OBSERVABILITY_SCHEMA_VERSION &&
           bytes_are_zero(
               reinterpret_cast<const uint8_t*>(cursor.reserved),
               sizeof(cursor.reserved));
}

bool metric_snapshot_header_valid(
    const rtfw_metric_snapshot& snapshot) noexcept {
    return snapshot.struct_size >= sizeof(rtfw_metric_snapshot) &&
           observability_metadata_header_valid(snapshot.metadata) &&
           bytes_are_zero(
               reinterpret_cast<const uint8_t*>(snapshot.reserved),
               sizeof(snapshot.reserved));
}

bool trace_cursor_header_valid(
    const rtfw_trace_cursor& cursor) noexcept {
    return cursor.struct_size >= sizeof(rtfw_trace_cursor) &&
           cursor.schema_version ==
               RTFW_OBSERVABILITY_SCHEMA_VERSION &&
           bytes_are_zero(
               reinterpret_cast<const uint8_t*>(cursor.reserved),
               sizeof(cursor.reserved));
}

bool trace_read_result_header_valid(
    const rtfw_trace_read_result& result) noexcept {
    return result.struct_size >=
               sizeof(rtfw_trace_read_result) &&
           result.reserved0 == 0u &&
           observability_metadata_header_valid(result.metadata) &&
           bytes_are_zero(
               reinterpret_cast<const uint8_t*>(result.reserved),
               sizeof(result.reserved));
}

bool artifact_write_result_header_valid(
    const rtfw_artifact_write_result& result) noexcept {
    return result.struct_size >= sizeof(result) &&
           result.reserved0 == 0u &&
           bytes_are_zero(
               reinterpret_cast<const uint8_t*>(result.reserved),
               sizeof(result.reserved));
}

bool checkpoint_metadata_header_valid(
    const rtfw_checkpoint_metadata& metadata) noexcept {
    return metadata.struct_size >= sizeof(metadata) &&
           metadata.schema_version ==
               RTFW_CHECKPOINT_SCHEMA_VERSION &&
           metadata.reserved0 == 0u &&
           bytes_are_zero(
               reinterpret_cast<const uint8_t*>(metadata.reserved),
               sizeof(metadata.reserved));
}

bool input_log_metadata_header_valid(
    const rtfw_input_log_metadata& metadata) noexcept {
    return metadata.struct_size >= sizeof(metadata) &&
           metadata.schema_version ==
               RTFW_INPUT_LOG_SCHEMA_VERSION &&
           metadata.reserved0 == 0u &&
           bytes_are_zero(
               reinterpret_cast<const uint8_t*>(metadata.reserved),
               sizeof(metadata.reserved));
}

bool replay_input_record_valid(
    const rtfw_replay_input_record& record) noexcept {
    return record.struct_size >= sizeof(record) &&
           record.delta_ns >= 0 &&
           record.has_deadline <= 1u &&
           bytes_are_zero(
               record.reserved0,
               sizeof(record.reserved0)) &&
           (record.has_deadline != 0u ||
            record.deadline_ns == 0u) &&
           (record.payload_size == 0 || record.payload) &&
           record.payload_size <=
               std::numeric_limits<std::size_t>::max() &&
           bytes_are_zero(
               reinterpret_cast<const uint8_t*>(record.reserved),
               sizeof(record.reserved));
}

bool replay_result_header_valid(
    const rtfw_replay_result& result) noexcept {
    return result.struct_size >= sizeof(result) &&
           result.reserved0 == 0u &&
           bytes_are_zero(
               reinterpret_cast<const uint8_t*>(result.reserved),
               sizeof(result.reserved));
}

bool to_cpp_config(
    const rtfw_config& source,
    rt::RuntimeConfig& target) noexcept {
    if (!config_header_valid(source) ||
        source.callback_capacity > std::numeric_limits<std::size_t>::max() ||
        source.scratch_bytes > std::numeric_limits<std::size_t>::max() ||
        source.trace_capacity > std::numeric_limits<std::size_t>::max() ||
        source.worker_count > std::numeric_limits<std::size_t>::max() ||
        source.executor_queue_capacity >
            std::numeric_limits<std::size_t>::max() ||
        source.scratch_alignment >
            std::numeric_limits<std::size_t>::max() ||
        source.task_scratch_bytes >
            std::numeric_limits<std::size_t>::max() ||
        source.task_scratch_slots >
            std::numeric_limits<std::size_t>::max() ||
        source.memory_budget_bytes >
            std::numeric_limits<std::size_t>::max() ||
        source.state_capacity >
            std::numeric_limits<std::size_t>::max() ||
        source.snapshot_max_bytes >
            std::numeric_limits<std::size_t>::max() ||
        source.replay_input_capacity >
            std::numeric_limits<std::size_t>::max() ||
        source.input_log_max_bytes >
            std::numeric_limits<std::size_t>::max() ||
        source.device_backend_capacity >
            std::numeric_limits<std::size_t>::max() ||
        source.device_buffer_capacity >
            std::numeric_limits<std::size_t>::max() ||
        source.device_outstanding_capacity >
            std::numeric_limits<std::size_t>::max() ||
        source.device_completion_batch >
            std::numeric_limits<std::size_t>::max()) {
        return false;
    }

    target.callback_capacity =
        static_cast<std::size_t>(source.callback_capacity);
    target.scratch_bytes = static_cast<std::size_t>(source.scratch_bytes);
    target.trace_capacity = static_cast<std::size_t>(source.trace_capacity);
    switch (source.numerical_mode) {
    case RTFW_NUMERICAL_PRECISE:
        target.numerical_mode = rt::NumericalMode::precise;
        break;
    case RTFW_NUMERICAL_FUSED_MULTIPLY_ADD:
        target.numerical_mode = rt::NumericalMode::fused_multiply_add;
        break;
    default:
        return false;
    }
    switch (source.executor_policy) {
    case RTFW_EXECUTOR_STATIC_DETERMINISTIC:
        target.executor_policy =
            rt::ExecutorPolicy::static_deterministic;
        break;
    case RTFW_EXECUTOR_BOUNDED_THROUGHPUT:
        target.executor_policy =
            rt::ExecutorPolicy::bounded_throughput;
        break;
    case RTFW_EXECUTOR_HOST_ADAPTER:
        target.executor_policy =
            rt::ExecutorPolicy::host_adapter;
        break;
    default:
        return false;
    }
    target.worker_count =
        static_cast<std::size_t>(source.worker_count);
    target.executor_queue_capacity =
        static_cast<std::size_t>(source.executor_queue_capacity);
    target.scratch_alignment =
        static_cast<std::size_t>(source.scratch_alignment);
    target.task_scratch_bytes =
        static_cast<std::size_t>(source.task_scratch_bytes);
    target.task_scratch_slots =
        static_cast<std::size_t>(source.task_scratch_slots);
    target.memory_budget_bytes =
        static_cast<std::size_t>(source.memory_budget_bytes);
    switch (source.overload_policy) {
    case RTFW_OVERLOAD_REJECT_SUBMISSION:
        target.overload_policy =
            rt::OverloadPolicy::reject_submission;
        break;
    case RTFW_OVERLOAD_FAIL_FRAME:
        target.overload_policy =
            rt::OverloadPolicy::fail_frame;
        break;
    default:
        return false;
    }
    target.watchdog_timeout_ns = source.watchdog_timeout_ns;
    target.watchdog_max_degradation_level =
        source.watchdog_max_degradation_level;
    switch (source.platform_preflight_mode) {
    case RTFW_PLATFORM_PREFLIGHT_DISABLED:
        target.platform_preflight_mode =
            rt::PlatformPreflightMode::disabled;
        break;
    case RTFW_PLATFORM_PREFLIGHT_STRICT:
        target.platform_preflight_mode =
            rt::PlatformPreflightMode::strict;
        break;
    default:
        return false;
    }
    switch (source.determinism_tier) {
    case RTFW_DETERMINISM_D0_UNSPECIFIED:
        target.determinism_tier =
            rt::DeterminismTier::unspecified;
        break;
    case RTFW_DETERMINISM_D1_SCHEDULE_INDEPENDENT:
        target.determinism_tier =
            rt::DeterminismTier::schedule_independent;
        break;
    case RTFW_DETERMINISM_D2_REPRODUCIBLE_BUILD:
        target.determinism_tier =
            rt::DeterminismTier::reproducible_build;
        break;
    case RTFW_DETERMINISM_D3_PORTABLE:
        target.determinism_tier =
            rt::DeterminismTier::portable_deterministic;
        break;
    default:
        return false;
    }
    target.state_capacity =
        static_cast<std::size_t>(source.state_capacity);
    target.snapshot_max_bytes =
        static_cast<std::size_t>(source.snapshot_max_bytes);
    target.replay_input_capacity =
        static_cast<std::size_t>(
            source.replay_input_capacity);
    target.input_log_max_bytes =
        static_cast<std::size_t>(
            source.input_log_max_bytes);
    target.device_backend_capacity =
        static_cast<std::size_t>(
            source.device_backend_capacity);
    target.device_buffer_capacity =
        static_cast<std::size_t>(
            source.device_buffer_capacity);
    target.device_outstanding_capacity =
        static_cast<std::size_t>(
            source.device_outstanding_capacity);
    target.device_completion_batch =
        static_cast<std::size_t>(
            source.device_completion_batch);
    std::copy(
        std::begin(source.workload_id),
        std::end(source.workload_id),
        target.workload_id.begin());
    return true;
}

void from_cpp_config(
    const rt::RuntimeConfig& source,
    rtfw_config& target) noexcept {
    target.callback_capacity = source.callback_capacity;
    target.scratch_bytes = source.scratch_bytes;
    target.trace_capacity = source.trace_capacity;
    target.numerical_mode =
        source.numerical_mode == rt::NumericalMode::fused_multiply_add
        ? RTFW_NUMERICAL_FUSED_MULTIPLY_ADD
        : RTFW_NUMERICAL_PRECISE;
    switch (source.executor_policy) {
    case rt::ExecutorPolicy::static_deterministic:
        target.executor_policy =
            RTFW_EXECUTOR_STATIC_DETERMINISTIC;
        break;
    case rt::ExecutorPolicy::bounded_throughput:
        target.executor_policy =
            RTFW_EXECUTOR_BOUNDED_THROUGHPUT;
        break;
    case rt::ExecutorPolicy::host_adapter:
        target.executor_policy = RTFW_EXECUTOR_HOST_ADAPTER;
        break;
    }
    target.worker_count = source.worker_count;
    target.executor_queue_capacity =
        source.executor_queue_capacity;
    target.scratch_alignment = source.scratch_alignment;
    target.task_scratch_bytes = source.task_scratch_bytes;
    target.task_scratch_slots = source.task_scratch_slots;
    target.memory_budget_bytes = source.memory_budget_bytes;
    target.overload_policy =
        source.overload_policy == rt::OverloadPolicy::fail_frame
        ? RTFW_OVERLOAD_FAIL_FRAME
        : RTFW_OVERLOAD_REJECT_SUBMISSION;
    target.watchdog_timeout_ns = source.watchdog_timeout_ns;
    target.watchdog_max_degradation_level =
        source.watchdog_max_degradation_level;
    target.platform_preflight_mode =
        source.platform_preflight_mode ==
            rt::PlatformPreflightMode::strict
        ? RTFW_PLATFORM_PREFLIGHT_STRICT
        : RTFW_PLATFORM_PREFLIGHT_DISABLED;
    target.determinism_tier =
        static_cast<std::uint32_t>(
            source.determinism_tier);
    target.state_capacity = source.state_capacity;
    target.snapshot_max_bytes =
        source.snapshot_max_bytes;
    target.replay_input_capacity =
        source.replay_input_capacity;
    target.input_log_max_bytes =
        source.input_log_max_bytes;
    target.device_backend_capacity =
        source.device_backend_capacity;
    target.device_buffer_capacity =
        source.device_buffer_capacity;
    target.device_outstanding_capacity =
        source.device_outstanding_capacity;
    target.device_completion_batch =
        source.device_completion_batch;
    std::copy(
        source.workload_id.begin(),
        source.workload_id.end(),
        std::begin(target.workload_id));
}

rtfw_runtime_state to_c_state(rt::RuntimeState state) noexcept {
    switch (state) {
    case rt::RuntimeState::configuring:
        return RTFW_STATE_CONFIGURING;
    case rt::RuntimeState::finalized:
        return RTFW_STATE_FINALIZED;
    case rt::RuntimeState::running:
        return RTFW_STATE_RUNNING;
    case rt::RuntimeState::stopped:
        return RTFW_STATE_STOPPED;
    }
    return RTFW_STATE_STOPPED;
}

void from_cpp_memory_plan(
    const rt::MemoryPlan& source,
    rtfw_memory_plan& target) noexcept {
    target.overload_policy =
        source.overload_policy == rt::OverloadPolicy::fail_frame
        ? RTFW_OVERLOAD_FAIL_FRAME
        : RTFW_OVERLOAD_REJECT_SUBMISSION;
    target.memory_budget_bytes = source.memory_budget_bytes;
    target.planned_bytes = source.planned_bytes;
    target.runtime_control_bytes = source.runtime_control_bytes;
    target.executor_control_bytes = source.executor_control_bytes;
    target.phase_count = source.phase_count;
    target.phase_scratch_bytes = source.phase_scratch_bytes;
    target.phase_scratch_stride = source.phase_scratch_stride;
    target.phase_scratch_total_bytes =
        source.phase_scratch_total_bytes;
    target.task_scratch_bytes = source.task_scratch_bytes;
    target.task_scratch_stride = source.task_scratch_stride;
    target.task_scratch_slots = source.task_scratch_slots;
    target.task_scratch_total_bytes =
        source.task_scratch_total_bytes;
    target.trace_capacity = source.trace_capacity;
    target.trace_slot_bytes = source.trace_slot_bytes;
    target.trace_storage_bytes = source.trace_storage_bytes;
    target.state_count = source.state_count;
    target.registered_state_bytes =
        source.registered_state_bytes;
    target.snapshot_max_bytes =
        source.snapshot_max_bytes;
    target.replay_input_capacity =
        source.replay_input_capacity;
    target.input_log_max_bytes =
        source.input_log_max_bytes;
    target.device_backend_count =
        source.device_backend_count;
    target.device_buffer_count =
        source.device_buffer_count;
    target.device_outstanding_capacity =
        source.device_outstanding_capacity;
    target.device_completion_batch =
        source.device_completion_batch;
    target.device_control_bytes =
        source.device_control_bytes;
    target.device_backend_reported_bytes =
        source.device_backend_reported_bytes;
    target.queue_slots = source.queue_slots;
    target.scratch_alignment = source.scratch_alignment;
}

void from_cpp_periodic_frame(
    const rt::PeriodicFrameResult& source,
    rtfw_periodic_frame_result& target) noexcept {
    std::memset(&target, 0, sizeof(target));
    target.struct_size = sizeof(target);
    target.status = to_c_status(source.status);
    target.frame_index = source.frame_index;
    target.release_ns = source.release_ns;
    target.wake_ns = source.wake_ns;
    target.start_ns = source.start_ns;
    target.finish_ns = source.finish_ns;
    target.slack_ns = source.slack_ns;
    target.deadline_missed = source.deadline_missed ? 1u : 0u;
    target.watchdog_fired = source.watchdog_fired ? 1u : 0u;
    target.degradation_level = source.degradation_level;
}

void from_cpp_periodic_result(
    const rt::PeriodicRunResult& source,
    rtfw_periodic_run_result& target) noexcept {
    target.frames_executed = source.frames_executed;
    target.deadline_misses = source.deadline_misses;
    target.watchdog_events = source.watchdog_events;
    target.final_degradation_level =
        source.final_degradation_level;
    target.first_release_ns = source.first_release_ns;
    target.next_release_ns = source.next_release_ns;
    from_cpp_periodic_frame(source.last_frame, target.last_frame);
}

void from_cpp_preflight(
    const rt::PlatformPreflightReport& source,
    rtfw_platform_preflight_report& target) noexcept {
    target.mode =
        source.mode == rt::PlatformPreflightMode::strict
        ? RTFW_PLATFORM_PREFLIGHT_STRICT
        : RTFW_PLATFORM_PREFLIGHT_DISABLED;
    target.passed = source.passed ? 1u : 0u;
    target.check_count = std::min<std::size_t>(
        source.check_count,
        RTFW_PLATFORM_CHECK_CAPACITY);
    for (std::size_t index = 0;
         index < target.check_count;
         ++index) {
        const auto& source_check = source.checks[index];
        auto& target_check = target.checks[index];
        target_check.id =
            static_cast<std::uint32_t>(source_check.id);
        target_check.status =
            static_cast<std::uint32_t>(source_check.status);
        target_check.system_error = source_check.system_error;
        std::snprintf(
            target_check.message,
            sizeof(target_check.message),
            "%s",
            source_check.message.data());
    }
}

void from_cpp_observability_metadata(
    const rt::ObservabilityMetadata& source,
    rtfw_observability_metadata& target) noexcept {
    target.schema_version = source.schema_version;
    target.runtime_version_major =
        source.runtime_version_major;
    target.runtime_version_minor =
        source.runtime_version_minor;
    target.runtime_version_patch =
        source.runtime_version_patch;
    target.trace_event_size = source.trace_event_size;
    target.metric_sample_size = source.metric_sample_size;
    target.metric_count = source.metric_count;
    target.config_id = source.config_id;
    target.runtime_id = source.runtime_id;
    target.trace_capacity = source.trace_capacity;
    std::copy(
        source.build_id.begin(),
        source.build_id.end(),
        std::begin(target.build_id));
    std::copy(
        source.workload_id.begin(),
        source.workload_id.end(),
        std::begin(target.workload_id));
}

void from_cpp_metric_sample(
    const rt::RuntimeMetricSample& source,
    rtfw_metric_sample& target) noexcept {
    target.id = static_cast<std::uint16_t>(source.id);
    target.kind = static_cast<std::uint8_t>(source.kind);
    target.value = source.value;
}

void from_cpp_metric_snapshot(
    const rt::RuntimeMetricSnapshot& source,
    rtfw_metric_snapshot& target) noexcept {
    target.window =
        source.window == rt::RuntimeMetricWindow::interval
        ? RTFW_METRIC_INTERVAL
        : RTFW_METRIC_CUMULATIVE;
    from_cpp_observability_metadata(
        source.metadata,
        target.metadata);
    target.snapshot_sequence = source.snapshot_sequence;
    target.window_start_ns = source.window_start_ns;
    target.window_end_ns = source.window_end_ns;
    target.sample_count = source.sample_count;
    const auto count = std::min<std::size_t>(
        source.sample_count,
        RTFW_RUNTIME_METRIC_COUNT);
    for (std::size_t index = 0; index < count; ++index) {
        from_cpp_metric_sample(
            source.samples[index],
            target.samples[index]);
    }
}

void to_cpp_metric_cursor(
    const rtfw_metric_cursor& source,
    rt::RuntimeMetricCursor& target) noexcept {
    target.schema_version = source.schema_version;
    target.reserved0 = 0;
    target.runtime_id = source.runtime_id;
    target.window_end_ns = source.window_end_ns;
    std::copy(
        std::begin(source.counters),
        std::end(source.counters),
        target.counters.begin());
}

void from_cpp_metric_cursor(
    const rt::RuntimeMetricCursor& source,
    rtfw_metric_cursor& target) noexcept {
    target.schema_version = source.schema_version;
    target.runtime_id = source.runtime_id;
    target.window_end_ns = source.window_end_ns;
    std::copy(
        source.counters.begin(),
        source.counters.end(),
        std::begin(target.counters));
}

void from_cpp_trace_event(
    const rt::RuntimeTraceEvent& source,
    rtfw_trace_event& target) noexcept {
    target.schema_version = source.schema_version;
    target.record_size = source.record_size;
    target.type = static_cast<std::uint16_t>(source.type);
    target.status = to_c_status(source.status);
    target.producer =
        static_cast<std::uint16_t>(source.producer);
    target.sequence = source.sequence;
    target.timestamp_ns = source.timestamp_ns;
    target.frame_index = source.frame_index;
    target.callback_index = source.callback_index;
    target.worker_index = source.worker_index;
    target.value = source.value;
}

void to_cpp_trace_cursor(
    const rtfw_trace_cursor& source,
    rt::RuntimeTraceCursor& target) noexcept {
    target.schema_version = source.schema_version;
    target.reserved0 = 0;
    target.runtime_id = source.runtime_id;
    target.next_sequence = source.next_sequence;
}

void from_cpp_trace_cursor(
    const rt::RuntimeTraceCursor& source,
    rtfw_trace_cursor& target) noexcept {
    target.schema_version = source.schema_version;
    target.runtime_id = source.runtime_id;
    target.next_sequence = source.next_sequence;
}

void from_cpp_trace_result(
    const rt::RuntimeTraceReadResult& source,
    rtfw_trace_read_result& target) noexcept {
    from_cpp_observability_metadata(
        source.metadata,
        target.metadata);
    target.first_sequence = source.first_sequence;
    target.next_sequence = source.next_sequence;
    target.events_read = source.events_read;
    target.lost_events = source.lost_events;
    target.remaining_sequence_count =
        source.remaining_sequence_count;
}

void from_cpp_artifact_write_result(
    const rt::ArtifactWriteResult& source,
    rtfw_artifact_write_result& target) noexcept {
    target.required_bytes = source.required_bytes;
    target.bytes_written = source.bytes_written;
    target.checksum = source.checksum;
}

void from_cpp_checkpoint_metadata(
    const rt::CheckpointMetadata& source,
    rtfw_checkpoint_metadata& target) noexcept {
    target.schema_version = source.schema_version;
    target.runtime_version_major =
        source.runtime_version_major;
    target.runtime_version_minor =
        source.runtime_version_minor;
    target.runtime_version_patch =
        source.runtime_version_patch;
    target.determinism_tier =
        static_cast<std::uint32_t>(
            source.determinism_tier);
    target.state_count = source.state_count;
    target.config_id = source.config_id;
    target.replay_id = source.replay_id;
    target.graph_id = source.graph_id;
    target.state_schema_id = source.state_schema_id;
    target.checkpoint_frame_index =
        source.checkpoint_frame_index;
    target.state_payload_bytes =
        source.state_payload_bytes;
    target.total_bytes = source.total_bytes;
    target.state_hash = source.state_hash;
    target.artifact_checksum =
        source.artifact_checksum;
    std::copy(
        source.build_id.begin(),
        source.build_id.end(),
        std::begin(target.build_id));
    std::copy(
        source.workload_id.begin(),
        source.workload_id.end(),
        std::begin(target.workload_id));
}

void from_cpp_input_log_metadata(
    const rt::InputLogMetadata& source,
    rtfw_input_log_metadata& target) noexcept {
    target.schema_version = source.schema_version;
    target.runtime_version_major =
        source.runtime_version_major;
    target.runtime_version_minor =
        source.runtime_version_minor;
    target.runtime_version_patch =
        source.runtime_version_patch;
    target.determinism_tier =
        static_cast<std::uint32_t>(
            source.determinism_tier);
    target.record_count = source.record_count;
    target.replay_id = source.replay_id;
    target.state_schema_id = source.state_schema_id;
    target.payload_bytes = source.payload_bytes;
    target.total_bytes = source.total_bytes;
    target.artifact_checksum =
        source.artifact_checksum;
    target.first_frame_index = source.first_frame_index;
    target.last_frame_index = source.last_frame_index;
    std::copy(
        source.workload_id.begin(),
        source.workload_id.end(),
        std::begin(target.workload_id));
}

void from_cpp_replay_result(
    const rt::ReplayResult& source,
    rtfw_replay_result& target) noexcept {
    target.checkpoint_frame_index =
        source.checkpoint_frame_index;
    target.first_frame_index = source.first_frame_index;
    target.last_frame_index = source.last_frame_index;
    target.records_processed = source.records_processed;
    target.frames_replayed = source.frames_replayed;
    target.final_state_hash = source.final_state_hash;
}

struct CCallback {
    rtfw_frame_callback callback = nullptr;
    void* user_data = nullptr;
};

struct CDeviceCallback {
    rtfw_device_command_callback callback = nullptr;
    void* user_data = nullptr;
};

[[nodiscard]] const rtfw_task_context* to_c_task_context(
    const rt::TaskContext& context) noexcept {
    return reinterpret_cast<const rtfw_task_context*>(&context);
}

struct CRangeInvocation {
    rtfw_range_callback callback = nullptr;
    void* user_data = nullptr;
};

rt::TaskResult invoke_c_range(
    void* opaque,
    const rt::TaskContext& context,
    const rt::TaskRange& range) {
    auto& invocation = *static_cast<CRangeInvocation*>(opaque);
    return invocation.callback(
               invocation.user_data,
               to_c_task_context(context),
               range.begin,
               range.end,
               range.task_index) == RTFW_CALLBACK_OK
        ? rt::TaskResult::ok
        : rt::TaskResult::error;
}

struct CReductionInvocation {
    rtfw_range_callback range_callback = nullptr;
    rtfw_reduction_callback combine_callback = nullptr;
    void* user_data = nullptr;
};

rt::TaskResult invoke_c_reduction_range(
    void* opaque,
    const rt::TaskContext& context,
    const rt::TaskRange& range) {
    auto& invocation = *static_cast<CReductionInvocation*>(opaque);
    return invocation.range_callback(
               invocation.user_data,
               to_c_task_context(context),
               range.begin,
               range.end,
               range.task_index) == RTFW_CALLBACK_OK
        ? rt::TaskResult::ok
        : rt::TaskResult::error;
}

rt::TaskResult invoke_c_reduction_combine(
    void* opaque,
    const rt::TaskContext& context,
    std::size_t left_task_index,
    std::size_t right_task_index) {
    auto& invocation = *static_cast<CReductionInvocation*>(opaque);
    return invocation.combine_callback(
               invocation.user_data,
               to_c_task_context(context),
               left_task_index,
               right_task_index) == RTFW_CALLBACK_OK
        ? rt::TaskResult::ok
        : rt::TaskResult::error;
}

rt::CallbackResult invoke_c_callback(
    void* opaque,
    const rt::CallbackContext& context) {
    auto* registration = static_cast<CCallback*>(opaque);
    rtfw_callback_context c_context{};
    c_context.struct_size = sizeof(c_context);
    c_context.numerical_mode =
        context.numerics.mode() == rt::NumericalMode::fused_multiply_add
        ? RTFW_NUMERICAL_FUSED_MULTIPLY_ADD
        : RTFW_NUMERICAL_PRECISE;
    c_context.degradation_level = context.degradation_level;
    c_context.frame_index = context.frame.frame_index;
    c_context.delta_ns = context.frame.delta.count();
    c_context.has_deadline = context.frame.deadline_ns ? 1u : 0u;
    c_context.deadline_ns = context.frame.deadline_ns.value_or(0);
    c_context.scratch = context.scratch.data();
    c_context.scratch_bytes = context.scratch.size();
    c_context.tasks = to_c_task_context(context.tasks);

    return registration->callback(registration->user_data, &c_context) ==
            RTFW_CALLBACK_OK
        ? rt::CallbackResult::ok
        : rt::CallbackResult::error;
}

rt::CallbackResult invoke_c_device_callback(
    void* opaque,
    const rt::DeviceCallbackContext& context,
    rt::DeviceSubmission& submission) {
    auto* registration = static_cast<CDeviceCallback*>(opaque);
    rtfw_callback_context c_context{};
    c_context.struct_size = sizeof(c_context);
    c_context.numerical_mode =
        context.numerics.mode() ==
                rt::NumericalMode::fused_multiply_add
        ? RTFW_NUMERICAL_FUSED_MULTIPLY_ADD
        : RTFW_NUMERICAL_PRECISE;
    c_context.degradation_level = context.degradation_level;
    c_context.frame_index = context.frame.frame_index;
    c_context.delta_ns = context.frame.delta.count();
    c_context.has_deadline =
        context.frame.deadline_ns ? 1u : 0u;
    c_context.deadline_ns =
        context.frame.deadline_ns.value_or(0);
    c_context.scratch = context.scratch.data();
    c_context.scratch_bytes = context.scratch.size();
    c_context.tasks = to_c_task_context(context.tasks);
    return registration->callback(
               registration->user_data,
               &c_context,
               &submission) == RTFW_CALLBACK_OK
        ? rt::CallbackResult::ok
        : rt::CallbackResult::error;
}

struct CPeriodicInvocation {
    rtfw_periodic_frame_callback callback = nullptr;
    void* user_data = nullptr;
};

rt::CallbackResult invoke_c_periodic_observer(
    void* opaque,
    const rt::PeriodicFrameResult& frame) {
    auto& invocation =
        *static_cast<CPeriodicInvocation*>(opaque);
    rtfw_periodic_frame_result c_frame;
    from_cpp_periodic_frame(frame, c_frame);
    return invocation.callback(
               invocation.user_data,
               &c_frame) == RTFW_CALLBACK_OK
        ? rt::CallbackResult::ok
        : rt::CallbackResult::error;
}

struct CReplayInvocation {
    rtfw_replay_input_callback callback = nullptr;
    void* user_data = nullptr;
};

rt::CallbackResult invoke_c_replay_input(
    void* opaque,
    const rt::ReplayInputView& input) {
    auto& invocation =
        *static_cast<CReplayInvocation*>(opaque);
    rtfw_replay_input_view c_input{};
    c_input.struct_size = sizeof(c_input);
    c_input.input_type = input.input_type;
    c_input.frame_index = input.frame.frame_index;
    c_input.delta_ns = input.frame.delta.count();
    c_input.has_deadline =
        input.frame.deadline_ns ? 1u : 0u;
    c_input.deadline_ns =
        input.frame.deadline_ns.value_or(0);
    c_input.payload = input.payload.data();
    c_input.payload_size = input.payload.size();
    return invocation.callback(
               invocation.user_data,
               &c_input) == RTFW_CALLBACK_OK
        ? rt::CallbackResult::ok
        : rt::CallbackResult::error;
}

} // namespace

struct rtfw_handle {
    rt::Runtime runtime;
    std::vector<std::unique_ptr<CCallback>> callbacks;
    std::vector<std::unique_ptr<CDeviceCallback>> device_callbacks;
    rtfw_host_executor c_host_executor{};
    std::array<char, 256> boundary_error{};

    static rt::Status submit_host_job(
        void* opaque,
        const rt::HostExecutorJob& job) noexcept {
        auto& handle = *static_cast<rtfw_handle*>(opaque);
        rtfw_host_job c_job{};
        c_job.struct_size = sizeof(c_job);
        c_job.execute = job.execute;
        c_job.execution_context = job.execution_context;
        c_job.completion_context = job.completion_context;
        c_job.completion_token = job.completion_token;
        c_job.scratch = job.scratch;
        c_job.scratch_bytes = job.scratch_bytes;
        const auto status = handle.c_host_executor.submit(
            handle.c_host_executor.user_data,
            &c_job);
        switch (status) {
        case RTFW_STATUS_OK:
            return rt::Status::ok;
        case RTFW_STATUS_QUEUE_FULL:
            return rt::Status::queue_full;
        case RTFW_STATUS_RESOURCE_EXHAUSTED:
            return rt::Status::resource_exhausted;
        default:
            return rt::Status::internal_error;
        }
    }

    static bool try_execute_one_host_job(void* opaque) noexcept {
        auto& handle = *static_cast<rtfw_handle*>(opaque);
        return handle.c_host_executor.try_execute_one(
                   handle.c_host_executor.user_data) != 0u;
    }

    void clear_boundary_error() noexcept {
        boundary_error[0] = '\0';
    }

    rtfw_status fail(rtfw_status status, const char* message) noexcept {
        std::snprintf(
            boundary_error.data(),
            boundary_error.size(),
            "%s",
            message ? message : "C ABI boundary error");
        return status;
    }
};

extern "C" {

RTFW_API uint32_t rt_version_major(void) {
    return RT_VERSION_MAJOR;
}

RTFW_API uint32_t rt_version_minor(void) {
    return RT_VERSION_MINOR;
}

RTFW_API uint32_t rt_version_patch(void) {
    return RT_VERSION_PATCH;
}

RTFW_API rt_capabilities_c rt_query_capabilities(void) {
    const auto capabilities = rt::query_capabilities();
    return {
        static_cast<uint8_t>(capabilities.compiled_graph),
        static_cast<uint8_t>(capabilities.host_driven_time),
        static_cast<uint8_t>(capabilities.unified_cpu_executor),
        static_cast<uint8_t>(capabilities.host_executor_adapter),
        static_cast<uint8_t>(capabilities.bounded_memory_plan),
        static_cast<uint8_t>(capabilities.self_paced_time),
        static_cast<uint8_t>(capabilities.frame_watchdog),
        static_cast<uint8_t>(capabilities.strict_platform_preflight),
        static_cast<uint8_t>(
            capabilities.versioned_observability),
        static_cast<uint8_t>(
            capabilities.deterministic_replay),
        static_cast<uint8_t>(
            capabilities.bounded_device_backend),
    };
}

RTFW_API rtfw_status rtfw_get_abi_info(
    rtfw_abi_info* out_info) {
    if (!out_info || !abi_info_header_valid(*out_info)) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    const auto struct_size = out_info->struct_size;
    std::memset(out_info, 0, sizeof(*out_info));
    out_info->struct_size = struct_size;
    out_info->abi_version = RTFW_C_ABI_VERSION;
    out_info->min_compatible_abi_version =
        RTFW_C_ABI_MIN_COMPATIBLE_VERSION;
    out_info->pointer_size = sizeof(void*);
    out_info->layout_fingerprint =
        RTFW_C_ABI_LAYOUT_FINGERPRINT;
    out_info->feature_flags =
        RTFW_ABI_FEATURE_HOST_EXECUTOR_ADAPTER |
        RTFW_ABI_FEATURE_DEVICE_BACKEND |
        RTFW_ABI_FEATURE_DETERMINISTIC_REPLAY;
    return RTFW_STATUS_OK;
}

RTFW_API rtfw_status rtfw_check_abi(
    uint32_t requested_abi_version,
    uint64_t requested_layout_fingerprint) {
    if (requested_abi_version < RTFW_C_ABI_MIN_COMPATIBLE_VERSION ||
        requested_abi_version > RTFW_C_ABI_VERSION ||
        requested_layout_fingerprint !=
            RTFW_C_ABI_LAYOUT_FINGERPRINT) {
        return RTFW_STATUS_INCOMPATIBLE_ABI;
    }
    return RTFW_STATUS_OK;
}

RTFW_API const char* rtfw_status_message(rtfw_status status) {
    switch (status) {
    case RTFW_STATUS_OK:
        return rt::status_message(rt::Status::ok);
    case RTFW_STATUS_INVALID_ARGUMENT:
        return rt::status_message(rt::Status::invalid_argument);
    case RTFW_STATUS_INVALID_STATE:
        return rt::status_message(rt::Status::invalid_state);
    case RTFW_STATUS_INVALID_CONFIG:
        return rt::status_message(rt::Status::invalid_config);
    case RTFW_STATUS_CAPACITY_EXCEEDED:
        return rt::status_message(rt::Status::capacity_exceeded);
    case RTFW_STATUS_CALLBACK_FAILED:
        return rt::status_message(rt::Status::callback_failed);
    case RTFW_STATUS_RESOURCE_EXHAUSTED:
        return rt::status_message(rt::Status::resource_exhausted);
    case RTFW_STATUS_INTERNAL_ERROR:
        return rt::status_message(rt::Status::internal_error);
    case RTFW_STATUS_INVALID_HANDLE:
        return rt::status_message(rt::Status::invalid_handle);
    case RTFW_STATUS_GRAPH_CYCLE:
        return rt::status_message(rt::Status::graph_cycle);
    case RTFW_STATUS_RESOURCE_CONFLICT:
        return rt::status_message(rt::Status::resource_conflict);
    case RTFW_STATUS_QUEUE_FULL:
        return rt::status_message(rt::Status::queue_full);
    case RTFW_STATUS_SCRATCH_EXHAUSTED:
        return rt::status_message(rt::Status::scratch_exhausted);
    case RTFW_STATUS_PLATFORM_PREFLIGHT_FAILED:
        return rt::status_message(
            rt::Status::platform_preflight_failed);
    case RTFW_STATUS_CLOCK_FAILURE:
        return rt::status_message(rt::Status::clock_failure);
    case RTFW_STATUS_INVALID_ARTIFACT:
        return rt::status_message(
            rt::Status::invalid_artifact);
    case RTFW_STATUS_INCOMPATIBLE_ARTIFACT:
        return rt::status_message(
            rt::Status::incompatible_artifact);
    case RTFW_STATUS_DEVICE_QUEUE_FULL:
        return rt::status_message(
            rt::Status::device_queue_full);
    case RTFW_STATUS_DEVICE_TIMEOUT:
        return rt::status_message(rt::Status::device_timeout);
    case RTFW_STATUS_DEVICE_ERROR:
        return rt::status_message(rt::Status::device_error);
    case RTFW_STATUS_DEVICE_LOST:
        return rt::status_message(rt::Status::device_lost);
    case RTFW_STATUS_DEVICE_CANCELED:
        return rt::status_message(rt::Status::device_canceled);
    case RTFW_STATUS_DEVICE_RESET_REQUIRED:
        return rt::status_message(
            rt::Status::device_reset_required);
    case RTFW_STATUS_INCOMPATIBLE_ABI:
        return rt::status_message(rt::Status::incompatible_abi);
    }
    return "unknown runtime status";
}

RTFW_API const char* rtfw_metric_name(rtfw_metric_id id) {
    rt::RuntimeMetricDefinition definition;
    if (!rt::runtime_metric_definition(
            static_cast<std::size_t>(id),
            definition)) {
        return "unknown";
    }
    return definition.name.data();
}

RTFW_API const char* rtfw_trace_event_name(
    rtfw_trace_event_type type) {
    return rt::runtime_trace_event_name(
        static_cast<rt::RuntimeTraceEventType>(type));
}

RTFW_API void rtfw_abi_info_init(rtfw_abi_info* info) {
    if (!info) {
        return;
    }
    std::memset(info, 0, sizeof(*info));
    info->struct_size = sizeof(*info);
}

RTFW_API void rtfw_config_init(rtfw_config* config) {
    if (!config) {
        return;
    }
    std::memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->abi_version = RTFW_C_ABI_VERSION;
    from_cpp_config(rt::RuntimeConfig{}, *config);
}

RTFW_API rtfw_status rtfw_config_set(
    rtfw_config* config,
    const char* key,
    const char* value) {
    if (!config || !key || !value || !config_header_valid(*config)) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }

    rt::RuntimeConfig typed{};
    if (!to_cpp_config(*config, typed)) {
        return RTFW_STATUS_INVALID_CONFIG;
    }
    const auto status =
        rt::set_runtime_config_value(typed, std::string_view(key), std::string_view(value));
    if (status == rt::Status::ok) {
        from_cpp_config(typed, *config);
    }
    return to_c_status(status);
}

RTFW_API void rtfw_frame_context_init(rtfw_frame_context* context) {
    if (!context) {
        return;
    }
    std::memset(context, 0, sizeof(*context));
    context->struct_size = sizeof(*context);
}

RTFW_API void rtfw_step_result_init(rtfw_step_result* result) {
    if (!result) {
        return;
    }
    std::memset(result, 0, sizeof(*result));
    result->struct_size = sizeof(*result);
}

RTFW_API void rtfw_periodic_config_init(
    rtfw_periodic_config* config) {
    if (!config) {
        return;
    }
    std::memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    const rt::PeriodicRunConfig defaults{};
    config->first_frame_index = defaults.first_frame_index;
    config->frame_count = defaults.frame_count;
    config->period_ns =
        static_cast<uint64_t>(defaults.period.count());
    config->relative_deadline_ns =
        static_cast<uint64_t>(
            defaults.relative_deadline.count());
}

RTFW_API void rtfw_periodic_run_result_init(
    rtfw_periodic_run_result* result) {
    if (!result) {
        return;
    }
    std::memset(result, 0, sizeof(*result));
    result->struct_size = sizeof(*result);
    result->last_frame.struct_size =
        sizeof(result->last_frame);
}

RTFW_API void rtfw_memory_plan_init(rtfw_memory_plan* plan) {
    if (!plan) {
        return;
    }
    std::memset(plan, 0, sizeof(*plan));
    plan->struct_size = sizeof(*plan);
}

RTFW_API void rtfw_platform_preflight_report_init(
    rtfw_platform_preflight_report* report) {
    if (!report) {
        return;
    }
    std::memset(report, 0, sizeof(*report));
    report->struct_size = sizeof(*report);
}

RTFW_API void rtfw_observability_metadata_init(
    rtfw_observability_metadata* metadata) {
    if (!metadata) {
        return;
    }
    std::memset(metadata, 0, sizeof(*metadata));
    metadata->struct_size = sizeof(*metadata);
    metadata->schema_version =
        RTFW_OBSERVABILITY_SCHEMA_VERSION;
}

RTFW_API void rtfw_metric_cursor_init(
    rtfw_metric_cursor* cursor) {
    if (!cursor) {
        return;
    }
    std::memset(cursor, 0, sizeof(*cursor));
    cursor->struct_size = sizeof(*cursor);
    cursor->schema_version =
        RTFW_OBSERVABILITY_SCHEMA_VERSION;
}

RTFW_API void rtfw_metric_snapshot_init(
    rtfw_metric_snapshot* snapshot) {
    if (!snapshot) {
        return;
    }
    std::memset(snapshot, 0, sizeof(*snapshot));
    snapshot->struct_size = sizeof(*snapshot);
    rtfw_observability_metadata_init(&snapshot->metadata);
}

RTFW_API void rtfw_trace_cursor_init(
    rtfw_trace_cursor* cursor) {
    if (!cursor) {
        return;
    }
    std::memset(cursor, 0, sizeof(*cursor));
    cursor->struct_size = sizeof(*cursor);
    cursor->schema_version =
        RTFW_OBSERVABILITY_SCHEMA_VERSION;
}

RTFW_API void rtfw_trace_read_result_init(
    rtfw_trace_read_result* result) {
    if (!result) {
        return;
    }
    std::memset(result, 0, sizeof(*result));
    result->struct_size = sizeof(*result);
    rtfw_observability_metadata_init(&result->metadata);
}

RTFW_API void rtfw_artifact_write_result_init(
    rtfw_artifact_write_result* result) {
    if (!result) {
        return;
    }
    std::memset(result, 0, sizeof(*result));
    result->struct_size = sizeof(*result);
}

RTFW_API void rtfw_checkpoint_metadata_init(
    rtfw_checkpoint_metadata* metadata) {
    if (!metadata) {
        return;
    }
    std::memset(metadata, 0, sizeof(*metadata));
    metadata->struct_size = sizeof(*metadata);
    metadata->schema_version =
        RTFW_CHECKPOINT_SCHEMA_VERSION;
}

RTFW_API void rtfw_input_log_metadata_init(
    rtfw_input_log_metadata* metadata) {
    if (!metadata) {
        return;
    }
    std::memset(metadata, 0, sizeof(*metadata));
    metadata->struct_size = sizeof(*metadata);
    metadata->schema_version =
        RTFW_INPUT_LOG_SCHEMA_VERSION;
}

RTFW_API void rtfw_replay_input_record_init(
    rtfw_replay_input_record* record) {
    if (!record) {
        return;
    }
    std::memset(record, 0, sizeof(*record));
    record->struct_size = sizeof(*record);
}

RTFW_API void rtfw_replay_result_init(
    rtfw_replay_result* result) {
    if (!result) {
        return;
    }
    std::memset(result, 0, sizeof(*result));
    result->struct_size = sizeof(*result);
}

RTFW_API void rtfw_device_health_init(
    rtfw_device_health* health) {
    if (!health) {
        return;
    }
    std::memset(health, 0, sizeof(*health));
    health->struct_size = sizeof(*health);
}

RTFW_API void rtfw_host_executor_init(
    rtfw_host_executor* executor) {
    if (!executor) {
        return;
    }
    std::memset(executor, 0, sizeof(*executor));
    executor->struct_size = sizeof(*executor);
    executor->abi_version = RTFW_C_ABI_VERSION;
}

RTFW_API rtfw_status rtfw_create(
    const rtfw_config* config,
    rtfw_handle** out_handle) {
    if (!out_handle) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    *out_handle = nullptr;

    rt::RuntimeConfig typed{};
    if (config && !to_cpp_config(*config, typed)) {
        return RTFW_STATUS_INVALID_CONFIG;
    }

    try {
        auto handle = std::make_unique<rtfw_handle>();
        const auto status = handle->runtime.configure(typed);
        if (status != rt::Status::ok) {
            return to_c_status(status);
        }
        handle->callbacks.reserve(typed.callback_capacity);
        handle->device_callbacks.reserve(
            typed.callback_capacity);
        *out_handle = handle.release();
        return RTFW_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return RTFW_STATUS_RESOURCE_EXHAUSTED;
    } catch (...) {
        return RTFW_STATUS_INTERNAL_ERROR;
    }
}

RTFW_API rtfw_status rtfw_set_host_executor(
    rtfw_handle* handle,
    const rtfw_host_executor* executor) {
    if (!handle || !executor ||
        !host_executor_header_valid(*executor)) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    if (handle->runtime.state() !=
        rt::RuntimeState::configuring) {
        return handle->fail(
            RTFW_STATUS_INVALID_STATE,
            "host executor attachment is frozen");
    }

    const rt::HostExecutorAdapter adapter{
        handle,
        static_cast<std::size_t>(executor->worker_count),
        static_cast<std::size_t>(executor->queue_capacity),
        &rtfw_handle::submit_host_job,
        &rtfw_handle::try_execute_one_host_job,
    };
    const auto status = handle->runtime.set_host_executor(adapter);
    if (status != rt::Status::ok) {
        return to_c_status(status);
    }
    handle->c_host_executor = *executor;
    handle->clear_boundary_error();
    return RTFW_STATUS_OK;
}

RTFW_API rtfw_status rtfw_register_phase(
    rtfw_handle* handle,
    const char* name,
    rtfw_frame_callback callback,
    void* user_data,
    rtfw_phase_id* out_phase) {
    if (!out_phase) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    *out_phase = RTFW_INVALID_PHASE_ID;
    if (!handle) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    if (!name || !callback) {
        return handle->fail(
            RTFW_STATUS_INVALID_ARGUMENT,
            "callback name and function are required");
    }
    if (handle->runtime.state() != rt::RuntimeState::configuring) {
        return handle->fail(
            RTFW_STATUS_INVALID_STATE,
            "callback registration is frozen");
    }
    if (handle->callbacks.size() >=
        handle->runtime.config().callback_capacity) {
        return handle->fail(
            RTFW_STATUS_CAPACITY_EXCEEDED,
            "configured callback capacity exceeded");
    }

    handle->clear_boundary_error();
    try {
        auto registration = std::make_unique<CCallback>();
        registration->callback = callback;
        registration->user_data = user_data;
        CCallback* stable_registration = registration.get();
        handle->callbacks.push_back(std::move(registration));

        rt::PhaseHandle phase;
        const auto status = handle->runtime.register_callback(
            rt::CallbackRegistration{
                std::string_view(name),
                &invoke_c_callback,
                stable_registration,
            },
            phase);
        if (status != rt::Status::ok) {
            handle->callbacks.pop_back();
            return to_c_status(status);
        }
        *out_phase = phase.value;
        handle->clear_boundary_error();
        return RTFW_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return handle->fail(
            RTFW_STATUS_RESOURCE_EXHAUSTED,
            "callback registration allocation failed");
    } catch (...) {
        return handle->fail(
            RTFW_STATUS_INTERNAL_ERROR,
            "unexpected callback registration failure");
    }
}

RTFW_API rtfw_status rtfw_register_callback(
    rtfw_handle* handle,
    const char* name,
    rtfw_frame_callback callback,
    void* user_data) {
    rtfw_phase_id ignored = RTFW_INVALID_PHASE_ID;
    return rtfw_register_phase(
        handle,
        name,
        callback,
        user_data,
        &ignored);
}

RTFW_API rtfw_status rtfw_register_device_backend(
    rtfw_handle* handle,
    const char* name,
    const rtfw_device_backend_api* backend,
    rtfw_device_backend_id* out_backend) {
    if (!out_backend) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    *out_backend = RTFW_INVALID_DEVICE_BACKEND_ID;
    if (!handle || !name || !backend ||
        backend->struct_size < sizeof(*backend)) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    handle->clear_boundary_error();
    rt::DeviceBackendHandle registered;
    const auto status =
        handle->runtime.register_device_backend(
            rt::DeviceBackendRegistration{
                std::string_view(name),
                *backend,
            },
            registered);
    if (status == rt::Status::ok) {
        *out_backend = registered.value;
    }
    return to_c_status(status);
}

RTFW_API rtfw_status rtfw_register_device_buffer(
    rtfw_handle* handle,
    const char* name,
    rtfw_device_backend_id backend,
    void* storage,
    uint64_t storage_bytes,
    rtfw_device_buffer_flags flags,
    rtfw_device_buffer_id* out_buffer) {
    if (!out_buffer) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    *out_buffer = RTFW_INVALID_DEVICE_BUFFER_ID;
    if (!handle || !name || !storage ||
        storage_bytes == 0 ||
        storage_bytes >
            std::numeric_limits<std::size_t>::max()) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    handle->clear_boundary_error();
    rt::DeviceBufferHandle registered;
    const auto status =
        handle->runtime.register_device_buffer(
            rt::DeviceBufferRegistration{
                std::string_view(name),
                rt::DeviceBackendHandle{backend},
                std::span<std::byte>(
                    static_cast<std::byte*>(storage),
                    static_cast<std::size_t>(storage_bytes)),
                flags,
            },
            registered);
    if (status == rt::Status::ok) {
        *out_buffer = registered.value;
    }
    return to_c_status(status);
}

RTFW_API rtfw_status rtfw_register_device_phase(
    rtfw_handle* handle,
    const char* name,
    rtfw_device_backend_id backend,
    rtfw_device_command_callback callback,
    void* user_data,
    rtfw_phase_id* out_phase) {
    if (!out_phase) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    *out_phase = RTFW_INVALID_PHASE_ID;
    if (!handle || !name || !callback) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    handle->clear_boundary_error();
    try {
        auto registration =
            std::make_unique<CDeviceCallback>();
        registration->callback = callback;
        registration->user_data = user_data;
        auto* stable_registration = registration.get();
        handle->device_callbacks.push_back(
            std::move(registration));

        rt::PhaseHandle phase;
        const auto status =
            handle->runtime.register_device_phase(
                rt::DevicePhaseRegistration{
                    std::string_view(name),
                    rt::DeviceBackendHandle{backend},
                    &invoke_c_device_callback,
                    stable_registration,
                },
                phase);
        if (status != rt::Status::ok) {
            handle->device_callbacks.pop_back();
            return to_c_status(status);
        }
        *out_phase = phase.value;
        return RTFW_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return handle->fail(
            RTFW_STATUS_RESOURCE_EXHAUSTED,
            "device phase registration allocation failed");
    } catch (...) {
        return handle->fail(
            RTFW_STATUS_INTERNAL_ERROR,
            "unexpected device phase registration failure");
    }
}

RTFW_API rtfw_status rtfw_register_resource(
    rtfw_handle* handle,
    const char* name,
    rtfw_resource_id* out_resource) {
    if (!out_resource) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    *out_resource = RTFW_INVALID_RESOURCE_ID;
    if (!handle) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    if (!name) {
        return handle->fail(
            RTFW_STATUS_INVALID_ARGUMENT,
            "resource name is required");
    }

    handle->clear_boundary_error();
    rt::ResourceHandle resource;
    const auto status =
        handle->runtime.register_resource(std::string_view(name), resource);
    if (status == rt::Status::ok) {
        *out_resource = resource.value;
        handle->clear_boundary_error();
    }
    return to_c_status(status);
}

RTFW_API rtfw_status rtfw_register_state(
    rtfw_handle* handle,
    const char* name,
    uint32_t schema_version,
    void* storage,
    uint64_t storage_bytes) {
    if (!handle || !name || !storage ||
        storage_bytes == 0 ||
        storage_bytes >
            std::numeric_limits<std::size_t>::max()) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    std::string_view state_name;
    if (!bounded_c_identifier(
            name,
            RTFW_REPLAY_IDENTIFIER_CAPACITY,
            state_name)) {
        return handle->fail(
            RTFW_STATUS_INVALID_ARGUMENT,
            "state name must be a bounded replay identifier");
    }
    handle->clear_boundary_error();
    return to_c_status(handle->runtime.register_state(
        rt::StateRegistration{
            state_name,
            schema_version,
            std::span<std::byte>(
                static_cast<std::byte*>(storage),
                static_cast<std::size_t>(storage_bytes)),
        }));
}

RTFW_API rtfw_status rtfw_add_dependency(
    rtfw_handle* handle,
    rtfw_phase_id prerequisite,
    rtfw_phase_id dependent) {
    if (!handle) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    handle->clear_boundary_error();
    return to_c_status(handle->runtime.add_dependency(
        rt::PhaseHandle{prerequisite},
        rt::PhaseHandle{dependent}));
}

RTFW_API rtfw_status rtfw_declare_resource_access(
    rtfw_handle* handle,
    rtfw_phase_id phase,
    rtfw_resource_id resource,
    rtfw_resource_access access) {
    if (!handle) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }

    rt::ResourceAccess cpp_access;
    switch (access) {
    case RTFW_RESOURCE_READ:
        cpp_access = rt::ResourceAccess::read;
        break;
    case RTFW_RESOURCE_WRITE:
        cpp_access = rt::ResourceAccess::write;
        break;
    default:
        return handle->fail(
            RTFW_STATUS_INVALID_ARGUMENT,
            "resource access mode is invalid");
    }

    handle->clear_boundary_error();
    return to_c_status(handle->runtime.declare_resource_access(
        rt::PhaseHandle{phase},
        rt::ResourceHandle{resource},
        cpp_access));
}

RTFW_API rtfw_status rtfw_parallel_for(
    const rtfw_task_context* context,
    uint64_t item_count,
    uint64_t grain_size,
    rtfw_range_callback callback,
    void* user_data) {
    if (!context || !callback ||
        item_count > std::numeric_limits<std::size_t>::max() ||
        grain_size > std::numeric_limits<std::size_t>::max()) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }

    const auto& cpp_context =
        *reinterpret_cast<const rt::TaskContext*>(context);
    CRangeInvocation invocation{callback, user_data};
    return to_c_status(cpp_context.parallel_for(
        static_cast<std::size_t>(item_count),
        static_cast<std::size_t>(grain_size),
        &invoke_c_range,
        &invocation));
}

RTFW_API rtfw_status rtfw_parallel_reduce(
    const rtfw_task_context* context,
    uint64_t item_count,
    uint64_t grain_size,
    rtfw_range_callback range_callback,
    rtfw_reduction_callback combine_callback,
    void* user_data) {
    if (!context || !range_callback || !combine_callback ||
        item_count > std::numeric_limits<std::size_t>::max() ||
        grain_size > std::numeric_limits<std::size_t>::max()) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }

    const auto& cpp_context =
        *reinterpret_cast<const rt::TaskContext*>(context);
    CReductionInvocation invocation{
        range_callback,
        combine_callback,
        user_data,
    };
    return to_c_status(cpp_context.parallel_reduce(
        static_cast<std::size_t>(item_count),
        static_cast<std::size_t>(grain_size),
        &invoke_c_reduction_range,
        &invoke_c_reduction_combine,
        &invocation));
}

RTFW_API rtfw_status rtfw_task_worker_index(
    const rtfw_task_context* context,
    uint64_t* out_worker_index) {
    if (!context || !out_worker_index) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    const auto& cpp_context =
        *reinterpret_cast<const rt::TaskContext*>(context);
    *out_worker_index =
        static_cast<uint64_t>(cpp_context.worker_index());
    return RTFW_STATUS_OK;
}

RTFW_API rtfw_status rtfw_task_scratch(
    const rtfw_task_context* context,
    void** out_scratch,
    uint64_t* out_scratch_bytes) {
    if (!context || !out_scratch || !out_scratch_bytes) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    const auto& cpp_context =
        *reinterpret_cast<const rt::TaskContext*>(context);
    const auto scratch = cpp_context.scratch();
    *out_scratch = scratch.data();
    *out_scratch_bytes =
        static_cast<uint64_t>(scratch.size());
    return RTFW_STATUS_OK;
}

RTFW_API rtfw_status rtfw_finalize(rtfw_handle* handle) {
    if (!handle) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    handle->clear_boundary_error();
    return to_c_status(handle->runtime.finalize());
}

RTFW_API rtfw_status rtfw_start(rtfw_handle* handle) {
    if (!handle) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    handle->clear_boundary_error();
    return to_c_status(handle->runtime.start());
}

RTFW_API rtfw_status rtfw_step(
    rtfw_handle* handle,
    const rtfw_frame_context* frame,
    rtfw_step_result* result) {
    if (!handle) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    if (!frame || !frame_header_valid(*frame)) {
        return handle->fail(
            RTFW_STATUS_INVALID_ARGUMENT,
            "frame context is missing, undersized, or has nonzero reserved fields");
    }
    if (result && !result_header_valid(*result)) {
        return handle->fail(
            RTFW_STATUS_INVALID_ARGUMENT,
            "step result is undersized or has nonzero reserved fields");
    }
    if (frame->delta_ns < 0 || frame->has_deadline > 1u) {
        return handle->fail(
            RTFW_STATUS_INVALID_ARGUMENT,
            "invalid frame delta or deadline flag");
    }

    rt::HostFrameContext cpp_frame{};
    cpp_frame.frame_index = frame->frame_index;
    cpp_frame.delta = std::chrono::nanoseconds(frame->delta_ns);
    if (frame->has_deadline) {
        cpp_frame.deadline_ns = frame->deadline_ns;
    }

    rt::StepResult cpp_result{};
    handle->clear_boundary_error();
    const auto status = handle->runtime.step(cpp_frame, &cpp_result);
    if (result) {
        const auto struct_size = result->struct_size;
        std::memset(result, 0, sizeof(*result));
        result->struct_size = struct_size;
        result->callbacks_executed = cpp_result.callbacks_executed;
        result->start_ns = cpp_result.start_ns;
        result->finish_ns = cpp_result.finish_ns;
        result->deadline_missed = cpp_result.deadline_missed ? 1u : 0u;
        result->watchdog_fired = cpp_result.watchdog_fired ? 1u : 0u;
        result->degradation_level = cpp_result.degradation_level;
    }
    return to_c_status(status);
}

RTFW_API rtfw_status rtfw_run_periodic(
    rtfw_handle* handle,
    const rtfw_periodic_config* config,
    rtfw_periodic_frame_callback observer,
    void* observer_data,
    rtfw_periodic_run_result* result) {
    if (!handle) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    if (!config || !periodic_config_header_valid(*config)) {
        return handle->fail(
            RTFW_STATUS_INVALID_ARGUMENT,
            "periodic config is missing, undersized, or has nonzero reserved fields");
    }
    if (result && !periodic_result_header_valid(*result)) {
        return handle->fail(
            RTFW_STATUS_INVALID_ARGUMENT,
            "periodic result is undersized or has nonzero reserved fields");
    }
    if (config->frame_count >
            std::numeric_limits<std::size_t>::max() ||
        config->period_ns >
            static_cast<uint64_t>(
                std::chrono::nanoseconds::max().count()) ||
        config->relative_deadline_ns >
            static_cast<uint64_t>(
                std::chrono::nanoseconds::max().count())) {
        return handle->fail(
            RTFW_STATUS_INVALID_ARGUMENT,
            "periodic count or duration exceeds the native runtime range");
    }

    rt::PeriodicRunConfig cpp_config;
    cpp_config.first_frame_index = config->first_frame_index;
    cpp_config.frame_count =
        static_cast<std::size_t>(config->frame_count);
    cpp_config.period =
        std::chrono::nanoseconds(
            static_cast<std::chrono::nanoseconds::rep>(
                config->period_ns));
    if (config->has_first_release != 0u) {
        cpp_config.first_release_ns =
            config->first_release_ns;
    }
    cpp_config.relative_deadline =
        std::chrono::nanoseconds(
            static_cast<std::chrono::nanoseconds::rep>(
                config->relative_deadline_ns));

    CPeriodicInvocation invocation{observer, observer_data};
    rt::PeriodicRunResult cpp_result;
    handle->clear_boundary_error();
    const auto status = handle->runtime.run_periodic(
        cpp_config,
        observer ? &invoke_c_periodic_observer : nullptr,
        observer ? &invocation : nullptr,
        &cpp_result);
    if (result) {
        const auto struct_size = result->struct_size;
        std::memset(result, 0, sizeof(*result));
        result->struct_size = struct_size;
        result->last_frame.struct_size =
            sizeof(result->last_frame);
        from_cpp_periodic_result(cpp_result, *result);
    }
    return to_c_status(status);
}

RTFW_API rtfw_status rtfw_stop(rtfw_handle* handle) {
    if (!handle) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    handle->clear_boundary_error();
    return to_c_status(handle->runtime.stop());
}

RTFW_API rtfw_status rtfw_get_device_health(
    rtfw_handle* handle,
    rtfw_device_backend_id backend,
    rtfw_device_health* out_health) {
    if (!handle || !out_health ||
        !device_health_header_valid(*out_health)) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    const auto requested_size = out_health->struct_size;
    rt::DeviceHealth health = rt::make_device_health();
    handle->clear_boundary_error();
    const auto status = handle->runtime.device_health(
        rt::DeviceBackendHandle{backend},
        health);
    std::memset(out_health, 0, sizeof(*out_health));
    *out_health = health;
    out_health->struct_size = requested_size;
    return to_c_status(status);
}

RTFW_API rtfw_status rtfw_reset_device(
    rtfw_handle* handle,
    rtfw_device_backend_id backend) {
    if (!handle) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    handle->clear_boundary_error();
    return to_c_status(handle->runtime.reset_device(
        rt::DeviceBackendHandle{backend}));
}

RTFW_API rtfw_status rtfw_get_state(
    const rtfw_handle* handle,
    rtfw_runtime_state* out_state) {
    if (!handle || !out_state) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    *out_state = to_c_state(handle->runtime.state());
    return RTFW_STATUS_OK;
}

RTFW_API rtfw_status rtfw_get_memory_plan(
    const rtfw_handle* handle,
    rtfw_memory_plan* out_plan) {
    if (!handle || !out_plan) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    if (!memory_plan_header_valid(*out_plan)) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }

    rt::MemoryPlan cpp_plan;
    if (!handle->runtime.memory_plan(cpp_plan)) {
        return RTFW_STATUS_INVALID_STATE;
    }
    const auto struct_size = out_plan->struct_size;
    std::memset(out_plan, 0, sizeof(*out_plan));
    out_plan->struct_size = struct_size;
    from_cpp_memory_plan(cpp_plan, *out_plan);
    return RTFW_STATUS_OK;
}

RTFW_API rtfw_status rtfw_get_platform_preflight_report(
    const rtfw_handle* handle,
    rtfw_platform_preflight_report* out_report) {
    if (!handle || !out_report ||
        !preflight_report_header_valid(*out_report)) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }

    rt::PlatformPreflightReport cpp_report;
    if (!handle->runtime.platform_preflight_report(cpp_report)) {
        return RTFW_STATUS_INVALID_STATE;
    }
    const auto struct_size = out_report->struct_size;
    std::memset(out_report, 0, sizeof(*out_report));
    out_report->struct_size = struct_size;
    from_cpp_preflight(cpp_report, *out_report);
    return RTFW_STATUS_OK;
}

RTFW_API rtfw_status rtfw_get_degradation_level(
    const rtfw_handle* handle,
    uint32_t* out_level) {
    if (!handle || !out_level) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    *out_level = handle->runtime.degradation_level();
    return RTFW_STATUS_OK;
}

RTFW_API rtfw_status rtfw_get_observability_metadata(
    rtfw_handle* handle,
    rtfw_observability_metadata* out_metadata) {
    if (!handle || !out_metadata ||
        !observability_metadata_header_valid(*out_metadata)) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }

    rt::ObservabilityMetadata cpp_metadata;
    handle->clear_boundary_error();
    const auto status =
        handle->runtime.observability_metadata(cpp_metadata);
    if (status != rt::Status::ok) {
        return to_c_status(status);
    }

    const auto struct_size = out_metadata->struct_size;
    std::memset(out_metadata, 0, sizeof(*out_metadata));
    out_metadata->struct_size = struct_size;
    from_cpp_observability_metadata(
        cpp_metadata,
        *out_metadata);
    return RTFW_STATUS_OK;
}

RTFW_API rtfw_status rtfw_get_metrics(
    rtfw_handle* handle,
    rtfw_metric_window window,
    rtfw_metric_cursor* cursor,
    rtfw_metric_snapshot* out_snapshot) {
    if (!handle || !out_snapshot ||
        !metric_snapshot_header_valid(*out_snapshot) ||
        (cursor && !metric_cursor_header_valid(*cursor))) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }

    rt::RuntimeMetricWindow cpp_window;
    switch (window) {
    case RTFW_METRIC_CUMULATIVE:
        cpp_window = rt::RuntimeMetricWindow::cumulative;
        break;
    case RTFW_METRIC_INTERVAL:
        if (!cursor) {
            return handle->fail(
                RTFW_STATUS_INVALID_ARGUMENT,
                "interval metrics require an initialized cursor");
        }
        cpp_window = rt::RuntimeMetricWindow::interval;
        break;
    default:
        return handle->fail(
            RTFW_STATUS_INVALID_ARGUMENT,
            "metric window is invalid");
    }

    rt::RuntimeMetricCursor cpp_cursor;
    if (cursor) {
        to_cpp_metric_cursor(*cursor, cpp_cursor);
    }
    rt::RuntimeMetricSnapshot cpp_snapshot;
    handle->clear_boundary_error();
    const auto status = handle->runtime.metrics_snapshot(
        cpp_window,
        cpp_window == rt::RuntimeMetricWindow::interval
            ? &cpp_cursor
            : nullptr,
        cpp_snapshot);
    if (status != rt::Status::ok) {
        return to_c_status(status);
    }

    if (cpp_window == rt::RuntimeMetricWindow::interval) {
        from_cpp_metric_cursor(cpp_cursor, *cursor);
    }
    const auto struct_size = out_snapshot->struct_size;
    const auto metadata_size =
        out_snapshot->metadata.struct_size;
    std::memset(out_snapshot, 0, sizeof(*out_snapshot));
    out_snapshot->struct_size = struct_size;
    out_snapshot->metadata.struct_size = metadata_size;
    from_cpp_metric_snapshot(cpp_snapshot, *out_snapshot);
    return RTFW_STATUS_OK;
}

RTFW_API rtfw_status rtfw_read_trace(
    rtfw_handle* handle,
    rtfw_trace_cursor* cursor,
    rtfw_trace_event* events,
    uint64_t event_capacity,
    rtfw_trace_read_result* out_result) {
    if (!handle || !cursor || !out_result ||
        !trace_cursor_header_valid(*cursor) ||
        !trace_read_result_header_valid(*out_result) ||
        event_capacity > std::numeric_limits<std::size_t>::max() ||
        (event_capacity != 0 && !events)) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }

    try {
        const auto staging_capacity =
            std::min<std::size_t>(
                static_cast<std::size_t>(event_capacity),
                handle->runtime.config().trace_capacity);
        std::vector<rt::RuntimeTraceEvent> cpp_events(
            staging_capacity);
        rt::RuntimeTraceCursor cpp_cursor;
        to_cpp_trace_cursor(*cursor, cpp_cursor);
        rt::RuntimeTraceReadResult cpp_result;
        handle->clear_boundary_error();
        const auto status = handle->runtime.read_trace(
            cpp_cursor,
            cpp_events,
            cpp_result);
        if (status != rt::Status::ok) {
            return to_c_status(status);
        }

        from_cpp_trace_cursor(cpp_cursor, *cursor);
        for (std::size_t index = 0;
             index < cpp_result.events_read;
             ++index) {
            std::memset(&events[index], 0, sizeof(events[index]));
            from_cpp_trace_event(
                cpp_events[index],
                events[index]);
        }

        const auto struct_size = out_result->struct_size;
        const auto metadata_size =
            out_result->metadata.struct_size;
        std::memset(out_result, 0, sizeof(*out_result));
        out_result->struct_size = struct_size;
        out_result->metadata.struct_size = metadata_size;
        from_cpp_trace_result(cpp_result, *out_result);
        return RTFW_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return handle->fail(
            RTFW_STATUS_RESOURCE_EXHAUSTED,
            "trace export staging allocation failed");
    } catch (...) {
        return handle->fail(
            RTFW_STATUS_INTERNAL_ERROR,
            "unexpected trace export failure");
    }
}

RTFW_API rtfw_status rtfw_checkpoint_size(
    rtfw_handle* handle,
    uint64_t* out_required_bytes) {
    if (!handle || !out_required_bytes) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    *out_required_bytes = 0;
    std::size_t required = 0;
    handle->clear_boundary_error();
    const auto status =
        handle->runtime.checkpoint_size(required);
    if (status == rt::Status::ok) {
        *out_required_bytes = required;
    }
    return to_c_status(status);
}

RTFW_API rtfw_status rtfw_checkpoint_write(
    rtfw_handle* handle,
    uint64_t checkpoint_frame_index,
    void* output,
    uint64_t output_bytes,
    rtfw_artifact_write_result* out_result) {
    if (!handle || !out_result ||
        !artifact_write_result_header_valid(*out_result) ||
        output_bytes >
            std::numeric_limits<std::size_t>::max() ||
        (output_bytes != 0 && !output)) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }

    rt::ArtifactWriteResult cpp_result;
    handle->clear_boundary_error();
    const auto status = handle->runtime.write_checkpoint(
        checkpoint_frame_index,
        std::span<std::byte>(
            static_cast<std::byte*>(output),
            static_cast<std::size_t>(output_bytes)),
        cpp_result);
    const auto struct_size = out_result->struct_size;
    std::memset(out_result, 0, sizeof(*out_result));
    out_result->struct_size = struct_size;
    from_cpp_artifact_write_result(
        cpp_result,
        *out_result);
    return to_c_status(status);
}

RTFW_API rtfw_status rtfw_checkpoint_inspect(
    const void* checkpoint,
    uint64_t checkpoint_bytes,
    rtfw_checkpoint_metadata* out_metadata) {
    if (!out_metadata ||
        !checkpoint_metadata_header_valid(*out_metadata) ||
        checkpoint_bytes >
            std::numeric_limits<std::size_t>::max() ||
        (checkpoint_bytes != 0 && !checkpoint)) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    rt::CheckpointMetadata cpp_metadata;
    const auto status = rt::inspect_checkpoint_artifact(
        std::span<const std::byte>(
            static_cast<const std::byte*>(checkpoint),
            static_cast<std::size_t>(checkpoint_bytes)),
        cpp_metadata);
    if (status != rt::Status::ok) {
        return to_c_status(status);
    }
    const auto struct_size = out_metadata->struct_size;
    std::memset(out_metadata, 0, sizeof(*out_metadata));
    out_metadata->struct_size = struct_size;
    from_cpp_checkpoint_metadata(
        cpp_metadata,
        *out_metadata);
    return RTFW_STATUS_OK;
}

RTFW_API rtfw_status rtfw_checkpoint_restore(
    rtfw_handle* handle,
    const void* checkpoint,
    uint64_t checkpoint_bytes,
    rtfw_checkpoint_metadata* out_metadata) {
    if (!handle ||
        checkpoint_bytes >
            std::numeric_limits<std::size_t>::max() ||
        (checkpoint_bytes != 0 && !checkpoint) ||
        (out_metadata &&
         !checkpoint_metadata_header_valid(*out_metadata))) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    rt::CheckpointMetadata cpp_metadata;
    handle->clear_boundary_error();
    const auto status = handle->runtime.restore_checkpoint(
        std::span<const std::byte>(
            static_cast<const std::byte*>(checkpoint),
            static_cast<std::size_t>(checkpoint_bytes)),
        out_metadata ? &cpp_metadata : nullptr);
    if (status != rt::Status::ok) {
        return to_c_status(status);
    }
    if (out_metadata) {
        const auto struct_size = out_metadata->struct_size;
        std::memset(out_metadata, 0, sizeof(*out_metadata));
        out_metadata->struct_size = struct_size;
        from_cpp_checkpoint_metadata(
            cpp_metadata,
            *out_metadata);
    }
    return RTFW_STATUS_OK;
}

RTFW_API rtfw_status rtfw_input_log_write(
    rtfw_handle* handle,
    const rtfw_replay_input_record* records,
    uint64_t record_count,
    void* output,
    uint64_t output_bytes,
    rtfw_artifact_write_result* out_result) {
    if (!handle || !out_result ||
        !artifact_write_result_header_valid(*out_result) ||
        record_count >
            std::numeric_limits<std::size_t>::max() ||
        output_bytes >
            std::numeric_limits<std::size_t>::max() ||
        (record_count != 0 && !records) ||
        (output_bytes != 0 && !output) ||
        record_count >
            handle->runtime.config().replay_input_capacity) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }

    try {
        std::vector<rt::ReplayInputRecord> cpp_records;
        cpp_records.reserve(
            static_cast<std::size_t>(record_count));
        for (std::size_t index = 0;
             index < record_count;
             ++index) {
            const auto& record = records[index];
            if (!replay_input_record_valid(record)) {
                return handle->fail(
                    RTFW_STATUS_INVALID_ARGUMENT,
                    "replay input record is malformed");
            }
            rt::HostFrameContext frame;
            frame.frame_index = record.frame_index;
            frame.delta =
                std::chrono::nanoseconds(record.delta_ns);
            if (record.has_deadline != 0u) {
                frame.deadline_ns = record.deadline_ns;
            }
            cpp_records.push_back(rt::ReplayInputRecord{
                frame,
                record.input_type,
                std::span<const std::byte>(
                    static_cast<const std::byte*>(
                        record.payload),
                    static_cast<std::size_t>(
                        record.payload_size)),
            });
        }

        rt::ArtifactWriteResult cpp_result;
        handle->clear_boundary_error();
        const auto status = handle->runtime.write_input_log(
            cpp_records,
            std::span<std::byte>(
                static_cast<std::byte*>(output),
                static_cast<std::size_t>(output_bytes)),
            cpp_result);
        const auto struct_size = out_result->struct_size;
        std::memset(out_result, 0, sizeof(*out_result));
        out_result->struct_size = struct_size;
        from_cpp_artifact_write_result(
            cpp_result,
            *out_result);
        return to_c_status(status);
    } catch (const std::bad_alloc&) {
        return handle->fail(
            RTFW_STATUS_RESOURCE_EXHAUSTED,
            "replay input staging allocation failed");
    } catch (...) {
        return handle->fail(
            RTFW_STATUS_INTERNAL_ERROR,
            "unexpected input-log encoding failure");
    }
}

RTFW_API rtfw_status rtfw_input_log_inspect(
    const void* input_log,
    uint64_t input_log_bytes,
    rtfw_input_log_metadata* out_metadata) {
    if (!out_metadata ||
        !input_log_metadata_header_valid(*out_metadata) ||
        input_log_bytes >
            std::numeric_limits<std::size_t>::max() ||
        (input_log_bytes != 0 && !input_log)) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    rt::InputLogMetadata cpp_metadata;
    const auto status = rt::inspect_input_log_artifact(
        std::span<const std::byte>(
            static_cast<const std::byte*>(input_log),
            static_cast<std::size_t>(input_log_bytes)),
        cpp_metadata);
    if (status != rt::Status::ok) {
        return to_c_status(status);
    }
    const auto struct_size = out_metadata->struct_size;
    std::memset(out_metadata, 0, sizeof(*out_metadata));
    out_metadata->struct_size = struct_size;
    from_cpp_input_log_metadata(
        cpp_metadata,
        *out_metadata);
    return RTFW_STATUS_OK;
}

RTFW_API rtfw_status rtfw_replay(
    rtfw_handle* handle,
    const void* checkpoint,
    uint64_t checkpoint_bytes,
    const void* input_log,
    uint64_t input_log_bytes,
    rtfw_replay_input_callback input_callback,
    void* input_user_data,
    rtfw_replay_result* out_result) {
    if (!handle ||
        checkpoint_bytes >
            std::numeric_limits<std::size_t>::max() ||
        input_log_bytes >
            std::numeric_limits<std::size_t>::max() ||
        (checkpoint_bytes != 0 && !checkpoint) ||
        (input_log_bytes != 0 && !input_log) ||
        (out_result &&
         !replay_result_header_valid(*out_result))) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }

    CReplayInvocation invocation{
        input_callback,
        input_user_data,
    };
    rt::ReplayResult cpp_result;
    handle->clear_boundary_error();
    const auto status = handle->runtime.replay(
        std::span<const std::byte>(
            static_cast<const std::byte*>(checkpoint),
            static_cast<std::size_t>(checkpoint_bytes)),
        std::span<const std::byte>(
            static_cast<const std::byte*>(input_log),
            static_cast<std::size_t>(input_log_bytes)),
        input_callback ? &invoke_c_replay_input : nullptr,
        &invocation,
        out_result ? &cpp_result : nullptr);
    if (out_result) {
        const auto struct_size = out_result->struct_size;
        std::memset(out_result, 0, sizeof(*out_result));
        out_result->struct_size = struct_size;
        from_cpp_replay_result(cpp_result, *out_result);
    }
    return to_c_status(status);
}

RTFW_API rtfw_status rtfw_registered_state_hash(
    rtfw_handle* handle,
    uint64_t* out_hash) {
    if (!handle || !out_hash) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    *out_hash = 0;
    handle->clear_boundary_error();
    return to_c_status(
        handle->runtime.registered_state_hash(*out_hash));
}

RTFW_API rtfw_status rtfw_now_ns(
    rtfw_handle* handle,
    uint64_t* out_now_ns) {
    if (!handle || !out_now_ns) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    *out_now_ns = handle->runtime.now_ns();
    return RTFW_STATUS_OK;
}

RTFW_API const char* rtfw_last_error(const rtfw_handle* handle) {
    if (!handle) {
        return "invalid runtime handle";
    }
    if (handle->boundary_error[0] != '\0') {
        return handle->boundary_error.data();
    }
    return handle->runtime.last_error().data();
}

RTFW_API void rtfw_destroy(rtfw_handle* handle) {
    if (!handle) {
        return;
    }
    const auto state = handle->runtime.state();
    if (state == rt::RuntimeState::finalized ||
        state == rt::RuntimeState::running) {
        handle->clear_boundary_error();
        if (handle->runtime.stop() != rt::Status::ok) {
            /*
             * ABI v8 has no status-bearing destroy function. Preserve the
             * handle and every borrowed backend/buffer lifetime on teardown
             * failure so a caller retaining the pointer can inspect
             * last_error(), retry stop(), and call destroy again. Deleting here
             * would falsely release runtime ownership after a backend
             * explicitly retained hardware resources.
             */
            return;
        }
    }
    delete handle;
}

} // extern "C"
