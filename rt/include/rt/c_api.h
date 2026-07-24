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
#define RTFW_C_ABI_VERSION 2u

typedef struct {
    uint8_t compiled_graph;
    uint8_t host_driven_time;
    uint8_t unified_cpu_executor;
    uint8_t bounded_memory_plan;
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
    uint8_t reserved1[7];
} rtfw_step_result;

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

/* Capability bytes are 0 or 1 and report completed target-path guarantees. */
RTFW_API uint32_t rt_version_major(void);
RTFW_API uint32_t rt_version_minor(void);
RTFW_API uint32_t rt_version_patch(void);
RTFW_API rt_capabilities_c rt_query_capabilities(void);
RTFW_API const char* rtfw_status_message(rtfw_status status);

/* Use the init functions below. All reserved members must remain zero. */
RTFW_API void rtfw_config_init(rtfw_config* config);
RTFW_API rtfw_status rtfw_config_set(
    rtfw_config* config,
    const char* key,
    const char* value);
RTFW_API void rtfw_frame_context_init(rtfw_frame_context* context);
RTFW_API void rtfw_step_result_init(rtfw_step_result* result);

/*
 * The runtime copies configuration and callback names. Callback user_data is
 * borrowed until the runtime is stopped/destroyed. Callback context and
 * scratch pointers are valid only for that synchronous callback invocation.
 * Lifecycle and graph functions are single-host-thread operations in ABI
 * version 1.
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
 * no helper thread or inline emergency path is created.
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
RTFW_API rtfw_status rtfw_finalize(rtfw_handle* handle);
RTFW_API rtfw_status rtfw_start(rtfw_handle* handle);
RTFW_API rtfw_status rtfw_step(
    rtfw_handle* handle,
    const rtfw_frame_context* frame,
    rtfw_step_result* result);
RTFW_API rtfw_status rtfw_stop(rtfw_handle* handle);
RTFW_API rtfw_status rtfw_get_state(
    const rtfw_handle* handle,
    rtfw_runtime_state* out_state);
RTFW_API rtfw_status rtfw_now_ns(
    rtfw_handle* handle,
    uint64_t* out_now_ns);
/* Returned text is library-owned; handle-specific text may change on a call. */
RTFW_API const char* rtfw_last_error(const rtfw_handle* handle);
RTFW_API void rtfw_destroy(rtfw_handle* handle);

#ifdef __cplusplus
}
#endif
