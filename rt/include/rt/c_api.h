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
#define RTFW_C_ABI_VERSION 1u

typedef struct {
    uint8_t compiled_graph;
    uint8_t host_driven_time;
    uint8_t bounded_memory_plan;
} rt_capabilities_c;

typedef struct rtfw_handle rtfw_handle;

typedef enum rtfw_status {
    RTFW_STATUS_OK = 0,
    RTFW_STATUS_INVALID_ARGUMENT = -1,
    RTFW_STATUS_INVALID_STATE = -2,
    RTFW_STATUS_INVALID_CONFIG = -3,
    RTFW_STATUS_CAPACITY_EXCEEDED = -4,
    RTFW_STATUS_CALLBACK_FAILED = -5,
    RTFW_STATUS_RESOURCE_EXHAUSTED = -6,
    RTFW_STATUS_INTERNAL_ERROR = -7,
    RTFW_STATUS_INVALID_HANDLE = -8,
    RTFW_STATUS_GRAPH_CYCLE = -9,
    RTFW_STATUS_RESOURCE_CONFLICT = -10
} rtfw_status;

typedef enum rtfw_runtime_state {
    RTFW_STATE_CONFIGURING = 0,
    RTFW_STATE_FINALIZED = 1,
    RTFW_STATE_RUNNING = 2,
    RTFW_STATE_STOPPED = 3
} rtfw_runtime_state;

typedef enum rtfw_numerical_mode {
    RTFW_NUMERICAL_PRECISE = 0,
    RTFW_NUMERICAL_FUSED_MULTIPLY_ADD = 1
} rtfw_numerical_mode;

typedef struct rtfw_config {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t callback_capacity;
    uint64_t scratch_bytes;
    uint64_t trace_capacity;
    uint32_t numerical_mode;
    uint32_t reserved;
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

typedef enum rtfw_callback_result {
    RTFW_CALLBACK_OK = 0,
    RTFW_CALLBACK_ERROR = 1
} rtfw_callback_result;

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
