#pragma once

#include <stddef.h>
#include <stdint.h>

#include <rtfw/version.h>

#if defined _WIN32 || defined __CYGWIN__
#  if defined RTFW_STATIC_DEFINE
#    define RTFW_API
#  elif defined RTFW_BUILD
#    define RTFW_API __declspec(dllexport)
#  else
#    define RTFW_API __declspec(dllimport)
#  endif
#else
#  define RTFW_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RT_VERSION_MAJOR RTFW_VERSION_MAJOR
#define RT_VERSION_MINOR RTFW_VERSION_MINOR
#define RT_VERSION_PATCH RTFW_VERSION_PATCH
#define RTFW_C_ABI_VERSION 5u

typedef struct {
    uint8_t compiled_graph;
    uint8_t host_driven_time;
    uint8_t unified_cpu_executor;
    uint8_t bounded_memory_plan;
    uint8_t self_paced_time;
    uint8_t frame_watchdog;
    uint8_t strict_platform_preflight;
    uint8_t versioned_observability;
} rt_capabilities_c;

typedef struct rtfw_handle rtfw_handle;
typedef struct rtfw_task_context rtfw_task_context;

/* All C ABI discriminators are fixed-width so malformed input is readable. */
typedef int32_t rtfw_status;
#define RTFW_STATUS_OK ((rtfw_status)0)
#define RTFW_STATUS_INVALID_ARGUMENT ((rtfw_status)-1)
#define RTFW_STATUS_INVALID_STATE ((rtfw_status)-2)
#define RTFW_STATUS_INVALID_CONFIG ((rtfw_status)-3)
#define RTFW_STATUS_CAPACITY_EXCEEDED ((rtfw_status)-4)
#define RTFW_STATUS_CALLBACK_FAILED ((rtfw_status)-5)
#define RTFW_STATUS_RESOURCE_EXHAUSTED ((rtfw_status)-6)
#define RTFW_STATUS_INTERNAL_ERROR ((rtfw_status)-7)
#define RTFW_STATUS_INVALID_HANDLE ((rtfw_status)-8)
#define RTFW_STATUS_GRAPH_CYCLE ((rtfw_status)-9)
#define RTFW_STATUS_RESOURCE_CONFLICT ((rtfw_status)-10)
#define RTFW_STATUS_QUEUE_FULL ((rtfw_status)-11)
#define RTFW_STATUS_SCRATCH_EXHAUSTED ((rtfw_status)-12)
#define RTFW_STATUS_PLATFORM_PREFLIGHT_FAILED ((rtfw_status)-13)
#define RTFW_STATUS_CLOCK_FAILURE ((rtfw_status)-14)

typedef uint32_t rtfw_runtime_state;
#define RTFW_STATE_CONFIGURING ((rtfw_runtime_state)0u)
#define RTFW_STATE_FINALIZED ((rtfw_runtime_state)1u)
#define RTFW_STATE_RUNNING ((rtfw_runtime_state)2u)
#define RTFW_STATE_STOPPED ((rtfw_runtime_state)3u)

typedef uint32_t rtfw_numerical_mode;
#define RTFW_NUMERICAL_PRECISE ((rtfw_numerical_mode)0u)
#define RTFW_NUMERICAL_FUSED_MULTIPLY_ADD ((rtfw_numerical_mode)1u)

/*
 * Fixed-width so malformed numeric input remains well-defined at the C
 * boundary. Policies are immutable after finalization.
 */
typedef uint32_t rtfw_executor_policy;
#define RTFW_EXECUTOR_STATIC_DETERMINISTIC ((rtfw_executor_policy)0u)
#define RTFW_EXECUTOR_BOUNDED_THROUGHPUT ((rtfw_executor_policy)1u)

typedef uint32_t rtfw_overload_policy;
#define RTFW_OVERLOAD_REJECT_SUBMISSION ((rtfw_overload_policy)0u)
#define RTFW_OVERLOAD_FAIL_FRAME ((rtfw_overload_policy)1u)

typedef uint32_t rtfw_platform_preflight_mode;
#define RTFW_PLATFORM_PREFLIGHT_DISABLED ((rtfw_platform_preflight_mode)0u)
#define RTFW_PLATFORM_PREFLIGHT_STRICT ((rtfw_platform_preflight_mode)1u)

typedef uint32_t rtfw_platform_check_id;
#define RTFW_PLATFORM_CHECK_ABSOLUTE_MONOTONIC_CLOCK \
    ((rtfw_platform_check_id)0u)
#define RTFW_PLATFORM_CHECK_REALTIME_KERNEL ((rtfw_platform_check_id)1u)
#define RTFW_PLATFORM_CHECK_MEMORY_LOCK_LIMIT ((rtfw_platform_check_id)2u)
#define RTFW_PLATFORM_CHECK_LOCKED_MEMORY ((rtfw_platform_check_id)3u)
#define RTFW_PLATFORM_CHECK_ISOLATED_CPU_AFFINITY \
    ((rtfw_platform_check_id)4u)
#define RTFW_PLATFORM_CHECK_REALTIME_SCHEDULER \
    ((rtfw_platform_check_id)5u)

typedef uint32_t rtfw_platform_check_status;
#define RTFW_PLATFORM_CHECK_PASSED ((rtfw_platform_check_status)0u)
#define RTFW_PLATFORM_CHECK_FAILED ((rtfw_platform_check_status)1u)
#define RTFW_PLATFORM_CHECK_UNSUPPORTED ((rtfw_platform_check_status)2u)

#define RTFW_PLATFORM_CHECK_CAPACITY 6u
#define RTFW_PLATFORM_CHECK_MESSAGE_CAPACITY 96u
#define RTFW_OBSERVABILITY_SCHEMA_VERSION 1u
#define RTFW_OBSERVABILITY_IDENTIFIER_CAPACITY 64u
#define RTFW_RUNTIME_METRIC_COUNT 22u
#define RTFW_INVALID_TRACE_INDEX UINT32_MAX

typedef struct rtfw_config {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t callback_capacity;
    uint64_t scratch_bytes;
    uint64_t trace_capacity;
    uint32_t numerical_mode;
    uint32_t executor_policy;
    uint64_t worker_count;
    uint64_t executor_queue_capacity;
    uint64_t scratch_alignment;
    uint64_t task_scratch_bytes;
    uint64_t task_scratch_slots;
    uint64_t memory_budget_bytes;
    uint32_t overload_policy;
    uint32_t platform_preflight_mode;
    uint64_t watchdog_timeout_ns;
    uint32_t watchdog_max_degradation_level;
    uint32_t reserved0;
    char workload_id[RTFW_OBSERVABILITY_IDENTIFIER_CAPACITY];
    uint64_t reserved[2];
} rtfw_config;

typedef struct rtfw_frame_context {
    uint32_t struct_size;
    uint32_t reserved0;
    uint64_t frame_index;
    int64_t delta_ns;
    uint8_t has_deadline;
    uint8_t reserved1[7];
    uint64_t deadline_ns;
} rtfw_frame_context;

typedef struct rtfw_callback_context {
    uint32_t struct_size;
    uint32_t numerical_mode;
    uint32_t degradation_level;
    uint32_t reserved0;
    uint64_t frame_index;
    int64_t delta_ns;
    uint8_t has_deadline;
    uint8_t reserved[7];
    uint64_t deadline_ns;
    void* scratch;
    uint64_t scratch_bytes;
    const rtfw_task_context* tasks;
} rtfw_callback_context;

typedef struct rtfw_step_result {
    uint32_t struct_size;
    uint32_t reserved0;
    uint64_t callbacks_executed;
    uint64_t start_ns;
    uint64_t finish_ns;
    uint8_t deadline_missed;
    uint8_t watchdog_fired;
    uint8_t reserved1[2];
    uint32_t degradation_level;
} rtfw_step_result;

typedef struct rtfw_periodic_config {
    uint32_t struct_size;
    uint32_t reserved0;
    uint64_t first_frame_index;
    uint64_t frame_count;
    uint64_t period_ns;
    uint8_t has_first_release;
    uint8_t reserved1[7];
    uint64_t first_release_ns;
    uint64_t relative_deadline_ns;
    uint64_t reserved[2];
} rtfw_periodic_config;

typedef struct rtfw_periodic_frame_result {
    uint32_t struct_size;
    int32_t status;
    uint64_t frame_index;
    uint64_t release_ns;
    uint64_t wake_ns;
    uint64_t start_ns;
    uint64_t finish_ns;
    int64_t slack_ns;
    uint8_t deadline_missed;
    uint8_t watchdog_fired;
    uint8_t reserved0[2];
    uint32_t degradation_level;
} rtfw_periodic_frame_result;

typedef struct rtfw_periodic_run_result {
    uint32_t struct_size;
    uint32_t reserved0;
    uint64_t frames_executed;
    uint64_t deadline_misses;
    uint64_t watchdog_events;
    uint32_t final_degradation_level;
    uint32_t reserved1;
    uint64_t first_release_ns;
    uint64_t next_release_ns;
    rtfw_periodic_frame_result last_frame;
    uint64_t reserved[2];
} rtfw_periodic_run_result;

typedef struct rtfw_memory_plan {
    uint32_t struct_size;
    uint32_t overload_policy;
    uint64_t memory_budget_bytes;
    uint64_t planned_bytes;
    uint64_t runtime_control_bytes;
    uint64_t executor_control_bytes;
    uint64_t phase_count;
    uint64_t phase_scratch_bytes;
    uint64_t phase_scratch_stride;
    uint64_t phase_scratch_total_bytes;
    uint64_t task_scratch_bytes;
    uint64_t task_scratch_stride;
    uint64_t task_scratch_slots;
    uint64_t task_scratch_total_bytes;
    uint64_t trace_capacity;
    uint64_t trace_slot_bytes;
    uint64_t trace_storage_bytes;
    uint64_t queue_slots;
    uint64_t scratch_alignment;
    uint64_t reserved[4];
} rtfw_memory_plan;

typedef struct rtfw_platform_check_result {
    uint32_t id;
    uint32_t status;
    int32_t system_error;
    uint32_t reserved0;
    char message[RTFW_PLATFORM_CHECK_MESSAGE_CAPACITY];
} rtfw_platform_check_result;

typedef struct rtfw_platform_preflight_report {
    uint32_t struct_size;
    uint32_t mode;
    uint8_t passed;
    uint8_t reserved0[7];
    uint64_t check_count;
    rtfw_platform_check_result checks[RTFW_PLATFORM_CHECK_CAPACITY];
    uint64_t reserved[2];
} rtfw_platform_preflight_report;

typedef uint16_t rtfw_trace_event_type;
#define RTFW_TRACE_FINALIZED ((rtfw_trace_event_type)1u)
#define RTFW_TRACE_STARTED ((rtfw_trace_event_type)2u)
#define RTFW_TRACE_PERIODIC_RELEASE ((rtfw_trace_event_type)3u)
#define RTFW_TRACE_PERIODIC_WAKE ((rtfw_trace_event_type)4u)
#define RTFW_TRACE_STEP_BEGIN ((rtfw_trace_event_type)5u)
#define RTFW_TRACE_CALLBACK_BEGIN ((rtfw_trace_event_type)6u)
#define RTFW_TRACE_CALLBACK_END ((rtfw_trace_event_type)7u)
#define RTFW_TRACE_WATCHDOG_FIRED ((rtfw_trace_event_type)8u)
#define RTFW_TRACE_DEGRADATION_APPLIED ((rtfw_trace_event_type)9u)
#define RTFW_TRACE_STEP_END ((rtfw_trace_event_type)10u)
#define RTFW_TRACE_STOPPED ((rtfw_trace_event_type)11u)

typedef uint16_t rtfw_trace_producer;
#define RTFW_TRACE_PRODUCER_HOST ((rtfw_trace_producer)0u)
#define RTFW_TRACE_PRODUCER_WORKER ((rtfw_trace_producer)1u)

typedef struct rtfw_trace_event {
    uint32_t schema_version;
    uint16_t record_size;
    uint16_t type;
    int32_t status;
    uint16_t producer;
    uint16_t reserved0;
    uint64_t sequence;
    uint64_t timestamp_ns;
    uint64_t frame_index;
    uint32_t callback_index;
    uint32_t worker_index;
    uint64_t value;
    uint64_t reserved1;
} rtfw_trace_event;

typedef uint8_t rtfw_metric_kind;
#define RTFW_METRIC_COUNTER ((rtfw_metric_kind)0u)
#define RTFW_METRIC_GAUGE ((rtfw_metric_kind)1u)

typedef uint32_t rtfw_metric_window;
#define RTFW_METRIC_CUMULATIVE ((rtfw_metric_window)0u)
#define RTFW_METRIC_INTERVAL ((rtfw_metric_window)1u)

typedef uint16_t rtfw_metric_id;
#define RTFW_METRIC_FRAMES_STARTED ((rtfw_metric_id)0u)
#define RTFW_METRIC_FRAMES_COMPLETED ((rtfw_metric_id)1u)
#define RTFW_METRIC_FRAMES_FAILED ((rtfw_metric_id)2u)
#define RTFW_METRIC_CALLBACKS_STARTED ((rtfw_metric_id)3u)
#define RTFW_METRIC_CALLBACKS_COMPLETED ((rtfw_metric_id)4u)
#define RTFW_METRIC_CALLBACK_FAILURES ((rtfw_metric_id)5u)
#define RTFW_METRIC_DEADLINE_MISSES ((rtfw_metric_id)6u)
#define RTFW_METRIC_WATCHDOG_EVENTS ((rtfw_metric_id)7u)
#define RTFW_METRIC_DEGRADATION_EVENTS ((rtfw_metric_id)8u)
#define RTFW_METRIC_PERIODIC_RELEASES ((rtfw_metric_id)9u)
#define RTFW_METRIC_PERIODIC_WAKES ((rtfw_metric_id)10u)
#define RTFW_METRIC_TRACE_EVENTS_EMITTED ((rtfw_metric_id)11u)
#define RTFW_METRIC_TRACE_EVENTS_OVERWRITTEN ((rtfw_metric_id)12u)
#define RTFW_METRIC_TRACE_EVENTS_DROPPED ((rtfw_metric_id)13u)
#define RTFW_METRIC_EXECUTOR_SUBMITTED_TASKS ((rtfw_metric_id)14u)
#define RTFW_METRIC_EXECUTOR_LOCAL_EXECUTIONS ((rtfw_metric_id)15u)
#define RTFW_METRIC_EXECUTOR_STEAL_ATTEMPTS ((rtfw_metric_id)16u)
#define RTFW_METRIC_EXECUTOR_SUCCESSFUL_STEALS ((rtfw_metric_id)17u)
#define RTFW_METRIC_EXECUTOR_QUEUE_REJECTIONS ((rtfw_metric_id)18u)
#define RTFW_METRIC_EXECUTOR_SCRATCH_EXHAUSTIONS ((rtfw_metric_id)19u)
#define RTFW_METRIC_EXECUTOR_WORKER_STARTS ((rtfw_metric_id)20u)
#define RTFW_METRIC_DEGRADATION_LEVEL ((rtfw_metric_id)21u)

typedef struct rtfw_observability_metadata {
    uint32_t struct_size;
    uint32_t schema_version;
    uint32_t runtime_version_major;
    uint32_t runtime_version_minor;
    uint32_t runtime_version_patch;
    uint32_t trace_event_size;
    uint32_t metric_sample_size;
    uint32_t metric_count;
    uint64_t config_id;
    uint64_t runtime_id;
    uint64_t trace_capacity;
    char build_id[RTFW_OBSERVABILITY_IDENTIFIER_CAPACITY];
    char workload_id[RTFW_OBSERVABILITY_IDENTIFIER_CAPACITY];
} rtfw_observability_metadata;

typedef struct rtfw_metric_sample {
    uint16_t id;
    uint8_t kind;
    uint8_t reserved0;
    uint32_t reserved1;
    uint64_t value;
} rtfw_metric_sample;

typedef struct rtfw_metric_cursor {
    uint32_t struct_size;
    uint32_t schema_version;
    uint64_t runtime_id;
    uint64_t window_end_ns;
    uint64_t counters[RTFW_RUNTIME_METRIC_COUNT];
    uint64_t reserved[2];
} rtfw_metric_cursor;

typedef struct rtfw_metric_snapshot {
    uint32_t struct_size;
    uint32_t window;
    rtfw_observability_metadata metadata;
    uint64_t snapshot_sequence;
    uint64_t window_start_ns;
    uint64_t window_end_ns;
    uint64_t sample_count;
    rtfw_metric_sample samples[RTFW_RUNTIME_METRIC_COUNT];
    uint64_t reserved[2];
} rtfw_metric_snapshot;

typedef struct rtfw_trace_cursor {
    uint32_t struct_size;
    uint32_t schema_version;
    uint64_t runtime_id;
    uint64_t next_sequence;
    uint64_t reserved[2];
} rtfw_trace_cursor;

typedef struct rtfw_trace_read_result {
    uint32_t struct_size;
    uint32_t reserved0;
    rtfw_observability_metadata metadata;
    uint64_t first_sequence;
    uint64_t next_sequence;
    uint64_t events_read;
    uint64_t lost_events;
    uint64_t remaining_sequence_count;
    uint64_t reserved[2];
} rtfw_trace_read_result;

typedef uint32_t rtfw_callback_result;
#define RTFW_CALLBACK_OK ((rtfw_callback_result)0u)
#define RTFW_CALLBACK_ERROR ((rtfw_callback_result)1u)

/* Opaque process-local values: do not persist, decode, or mix their kinds. */
typedef uint64_t rtfw_phase_id;
typedef uint64_t rtfw_resource_id;

#define RTFW_INVALID_PHASE_ID UINT64_MAX
#define RTFW_INVALID_RESOURCE_ID UINT64_MAX

/*
 * Fixed-width rather than an enum so the C boundary can validate malformed
 * numeric input without first loading an out-of-range enum value.
 */
typedef uint32_t rtfw_resource_access;
#define RTFW_RESOURCE_READ ((rtfw_resource_access)0u)
#define RTFW_RESOURCE_WRITE ((rtfw_resource_access)1u)

typedef rtfw_callback_result (*rtfw_frame_callback)(
    void* user_data,
    const rtfw_callback_context* context);

typedef rtfw_callback_result (*rtfw_range_callback)(
    void* user_data,
    const rtfw_task_context* context,
    uint64_t begin,
    uint64_t end,
    uint64_t task_index);

typedef rtfw_callback_result (*rtfw_reduction_callback)(
    void* user_data,
    const rtfw_task_context* context,
    uint64_t left_task_index,
    uint64_t right_task_index);

typedef rtfw_callback_result (*rtfw_periodic_frame_callback)(
    void* user_data,
    const rtfw_periodic_frame_result* frame);

/* Capability bytes are 0 or 1 and report completed target-path guarantees. */
RTFW_API uint32_t rt_version_major(void);
RTFW_API uint32_t rt_version_minor(void);
RTFW_API uint32_t rt_version_patch(void);
RTFW_API rt_capabilities_c rt_query_capabilities(void);
RTFW_API const char* rtfw_status_message(rtfw_status status);
RTFW_API const char* rtfw_metric_name(rtfw_metric_id id);
RTFW_API const char* rtfw_trace_event_name(
    rtfw_trace_event_type type);

/* Use the init functions below. All reserved members must remain zero. */
RTFW_API void rtfw_config_init(rtfw_config* config);
RTFW_API rtfw_status rtfw_config_set(
    rtfw_config* config,
    const char* key,
    const char* value);
RTFW_API void rtfw_frame_context_init(rtfw_frame_context* context);
RTFW_API void rtfw_step_result_init(rtfw_step_result* result);
RTFW_API void rtfw_periodic_config_init(rtfw_periodic_config* config);
RTFW_API void rtfw_periodic_run_result_init(
    rtfw_periodic_run_result* result);
RTFW_API void rtfw_memory_plan_init(rtfw_memory_plan* plan);
RTFW_API void rtfw_platform_preflight_report_init(
    rtfw_platform_preflight_report* report);
RTFW_API void rtfw_observability_metadata_init(
    rtfw_observability_metadata* metadata);
RTFW_API void rtfw_metric_cursor_init(
    rtfw_metric_cursor* cursor);
RTFW_API void rtfw_metric_snapshot_init(
    rtfw_metric_snapshot* snapshot);
RTFW_API void rtfw_trace_cursor_init(
    rtfw_trace_cursor* cursor);
RTFW_API void rtfw_trace_read_result_init(
    rtfw_trace_read_result* result);

/*
 * The runtime copies configuration and callback names. Callback user_data is
 * borrowed until the runtime is stopped/destroyed. Callback context and
 * scratch pointers are valid only for that synchronous callback invocation.
 * Lifecycle and graph functions are single-host-thread operations in ABI
 * version 5.
 */
RTFW_API rtfw_status rtfw_create(
    const rtfw_config* config,
    rtfw_handle** out_handle);
RTFW_API rtfw_status rtfw_register_callback(
    rtfw_handle* handle,
    const char* name,
    rtfw_frame_callback callback,
    void* user_data);
/* Preferred M2 registration form: returns an instance-local phase ID. */
RTFW_API rtfw_status rtfw_register_phase(
    rtfw_handle* handle,
    const char* name,
    rtfw_frame_callback callback,
    void* user_data,
    rtfw_phase_id* out_phase);
RTFW_API rtfw_status rtfw_register_resource(
    rtfw_handle* handle,
    const char* name,
    rtfw_resource_id* out_resource);
RTFW_API rtfw_status rtfw_add_dependency(
    rtfw_handle* handle,
    rtfw_phase_id prerequisite,
    rtfw_phase_id dependent);
RTFW_API rtfw_status rtfw_declare_resource_access(
    rtfw_handle* handle,
    rtfw_phase_id phase,
    rtfw_resource_id resource,
    rtfw_resource_access access);
/*
 * Nested work uses the runtime's existing fixed worker team and waits
 * synchronously. A full/busy bounded queue returns RTFW_STATUS_QUEUE_FULL;
 * unavailable task scratch returns RTFW_STATUS_SCRATCH_EXHAUSTED. The
 * configured overload policy controls whether rejection also fails the active
 * frame. No helper thread or inline emergency path is created.
 */
RTFW_API rtfw_status rtfw_parallel_for(
    const rtfw_task_context* context,
    uint64_t item_count,
    uint64_t grain_size,
    rtfw_range_callback callback,
    void* user_data);
RTFW_API rtfw_status rtfw_parallel_reduce(
    const rtfw_task_context* context,
    uint64_t item_count,
    uint64_t grain_size,
    rtfw_range_callback range_callback,
    rtfw_reduction_callback combine_callback,
    void* user_data);
RTFW_API rtfw_status rtfw_task_worker_index(
    const rtfw_task_context* context,
    uint64_t* out_worker_index);
/*
 * Returns storage exclusively owned by this callback invocation. Contents are
 * unspecified on entry; the pointer must not escape the callback.
 */
RTFW_API rtfw_status rtfw_task_scratch(
    const rtfw_task_context* context,
    void** out_scratch,
    uint64_t* out_scratch_bytes);
RTFW_API rtfw_status rtfw_finalize(rtfw_handle* handle);
RTFW_API rtfw_status rtfw_start(rtfw_handle* handle);
RTFW_API rtfw_status rtfw_step(
    rtfw_handle* handle,
    const rtfw_frame_context* frame,
    rtfw_step_result* result);
/*
 * Executes a finite runtime-paced loop on the calling frame thread. Release
 * timestamps are absolute and never drift after a late frame.
 */
RTFW_API rtfw_status rtfw_run_periodic(
    rtfw_handle* handle,
    const rtfw_periodic_config* config,
    rtfw_periodic_frame_callback observer,
    void* observer_data,
    rtfw_periodic_run_result* result);
RTFW_API rtfw_status rtfw_stop(rtfw_handle* handle);
RTFW_API rtfw_status rtfw_get_state(
    const rtfw_handle* handle,
    rtfw_runtime_state* out_state);
/*
 * Available after finalization. Initialize out_plan and keep reserved fields
 * zero.
 */
RTFW_API rtfw_status rtfw_get_memory_plan(
    const rtfw_handle* handle,
    rtfw_memory_plan* out_plan);
/* Available after start is attempted, including strict preflight failure. */
RTFW_API rtfw_status rtfw_get_platform_preflight_report(
    const rtfw_handle* handle,
    rtfw_platform_preflight_report* out_report);
RTFW_API rtfw_status rtfw_get_degradation_level(
    const rtfw_handle* handle,
    uint32_t* out_level);
/*
 * Observability reads are non-RT host operations and reject an active frame
 * or periodic loop. Interval metrics require a caller-owned cursor; trace
 * output storage is caller-owned and may be read in bounded batches. Fresh
 * cursors must come from their matching init functions.
 */
RTFW_API rtfw_status rtfw_get_observability_metadata(
    rtfw_handle* handle,
    rtfw_observability_metadata* out_metadata);
RTFW_API rtfw_status rtfw_get_metrics(
    rtfw_handle* handle,
    rtfw_metric_window window,
    rtfw_metric_cursor* cursor,
    rtfw_metric_snapshot* out_snapshot);
RTFW_API rtfw_status rtfw_read_trace(
    rtfw_handle* handle,
    rtfw_trace_cursor* cursor,
    rtfw_trace_event* events,
    uint64_t event_capacity,
    rtfw_trace_read_result* out_result);
RTFW_API rtfw_status rtfw_now_ns(
    rtfw_handle* handle,
    uint64_t* out_now_ns);
/* Returned text is library-owned; handle-specific text may change on a call. */
RTFW_API const char* rtfw_last_error(const rtfw_handle* handle);
RTFW_API void rtfw_destroy(rtfw_handle* handle);

#ifdef __cplusplus
}
#endif
