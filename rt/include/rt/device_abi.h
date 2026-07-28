#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTFW_DEVICE_ABI_VERSION 1u
#define RTFW_DEVICE_IDENTIFIER_CAPACITY 64u
#define RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY 128u
#define RTFW_DEVICE_BUFFER_REF_CAPACITY 8u

typedef int32_t rtfw_device_status;
#define RTFW_DEVICE_STATUS_OK ((rtfw_device_status)0)
#define RTFW_DEVICE_STATUS_INVALID_ARGUMENT ((rtfw_device_status)-1)
#define RTFW_DEVICE_STATUS_INVALID_STATE ((rtfw_device_status)-2)
#define RTFW_DEVICE_STATUS_QUEUE_FULL ((rtfw_device_status)-3)
#define RTFW_DEVICE_STATUS_TIMEOUT ((rtfw_device_status)-4)
#define RTFW_DEVICE_STATUS_ERROR ((rtfw_device_status)-5)
#define RTFW_DEVICE_STATUS_LOST ((rtfw_device_status)-6)
#define RTFW_DEVICE_STATUS_CANCELED ((rtfw_device_status)-7)
#define RTFW_DEVICE_STATUS_UNSUPPORTED ((rtfw_device_status)-8)
#define RTFW_DEVICE_STATUS_RESOURCE_EXHAUSTED ((rtfw_device_status)-9)
#define RTFW_DEVICE_STATUS_INTERNAL_ERROR ((rtfw_device_status)-10)
#define RTFW_DEVICE_STATUS_RESET_REQUIRED ((rtfw_device_status)-11)

typedef uint32_t rtfw_device_health_state;
#define RTFW_DEVICE_HEALTH_SHUTDOWN ((rtfw_device_health_state)0u)
#define RTFW_DEVICE_HEALTH_HEALTHY ((rtfw_device_health_state)1u)
#define RTFW_DEVICE_HEALTH_DEGRADED ((rtfw_device_health_state)2u)
#define RTFW_DEVICE_HEALTH_RESET_REQUIRED ((rtfw_device_health_state)3u)
#define RTFW_DEVICE_HEALTH_LOST ((rtfw_device_health_state)4u)

typedef uint32_t rtfw_device_buffer_flags;
#define RTFW_DEVICE_BUFFER_HOST_READ \
    ((rtfw_device_buffer_flags)(1u << 0u))
#define RTFW_DEVICE_BUFFER_HOST_WRITE \
    ((rtfw_device_buffer_flags)(1u << 1u))
#define RTFW_DEVICE_BUFFER_DEVICE_READ \
    ((rtfw_device_buffer_flags)(1u << 2u))
#define RTFW_DEVICE_BUFFER_DEVICE_WRITE \
    ((rtfw_device_buffer_flags)(1u << 3u))

typedef uint32_t rtfw_device_buffer_access;
#define RTFW_DEVICE_ACCESS_READ ((rtfw_device_buffer_access)1u)
#define RTFW_DEVICE_ACCESS_WRITE ((rtfw_device_buffer_access)2u)
#define RTFW_DEVICE_ACCESS_READ_WRITE ((rtfw_device_buffer_access)3u)

typedef struct rtfw_device_capabilities {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t max_in_flight;
    uint64_t max_registered_buffers;
    uint64_t max_buffer_bytes;
    uint32_t inline_payload_capacity;
    uint32_t buffer_ref_capacity;
    uint8_t supports_cancel;
    uint8_t supports_reset;
    uint8_t deterministic_mock;
    uint8_t reserved0;
    char backend_id[RTFW_DEVICE_IDENTIFIER_CAPACITY];
    uint64_t control_storage_bytes;
    uint64_t reserved[4];
} rtfw_device_capabilities;

typedef struct rtfw_device_init_config {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t requested_in_flight;
    uint64_t requested_registered_buffers;
    uint64_t reserved[4];
} rtfw_device_init_config;

typedef struct rtfw_device_buffer_registration {
    uint32_t struct_size;
    uint32_t flags;
    void* data;
    uint64_t bytes;
    char name[RTFW_DEVICE_IDENTIFIER_CAPACITY];
    uint64_t reserved[4];
} rtfw_device_buffer_registration;

typedef struct rtfw_device_buffer_ref {
    uint64_t buffer_token;
    uint32_t access;
    uint32_t reserved0;
    uint64_t offset;
    uint64_t bytes;
} rtfw_device_buffer_ref;

typedef struct rtfw_device_submission {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t submission_id;
    uint64_t frame_index;
    uint64_t timeout_ns;
    uint32_t opcode;
    uint32_t flags;
    uint32_t payload_size;
    uint32_t buffer_count;
    uint8_t payload[RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY];
    rtfw_device_buffer_ref buffers[RTFW_DEVICE_BUFFER_REF_CAPACITY];
    uint64_t reserved[4];
} rtfw_device_submission;

typedef struct rtfw_device_completion {
    uint32_t struct_size;
    int32_t status;
    uint64_t submission_id;
    uint64_t device_timestamp_ns;
    uint64_t value;
    uint64_t reserved[4];
} rtfw_device_completion;

typedef struct rtfw_device_health {
    uint32_t struct_size;
    uint32_t state;
    int32_t last_status;
    uint32_t reserved0;
    uint64_t generation;
    uint64_t submissions;
    uint64_t completions;
    uint64_t queue_rejections;
    uint64_t timeouts;
    uint64_t errors;
    uint64_t losses;
    uint64_t cancellations;
    uint64_t resets;
    uint64_t outstanding;
    uint64_t reserved[4];
} rtfw_device_health;

typedef rtfw_device_status (*rtfw_device_get_capabilities_fn)(
    void* instance,
    rtfw_device_capabilities* capabilities);
typedef rtfw_device_status (*rtfw_device_initialize_fn)(
    void* instance,
    const rtfw_device_init_config* config);
typedef rtfw_device_status (*rtfw_device_register_buffer_fn)(
    void* instance,
    const rtfw_device_buffer_registration* registration,
    uint64_t* out_buffer_token);
typedef rtfw_device_status (*rtfw_device_unregister_buffer_fn)(
    void* instance,
    uint64_t buffer_token);
typedef rtfw_device_status (*rtfw_device_submit_fn)(
    void* instance,
    const rtfw_device_submission* submission);
typedef rtfw_device_status (*rtfw_device_poll_fn)(
    void* instance,
    rtfw_device_completion* completions,
    uint64_t completion_capacity,
    uint64_t* out_completion_count);
typedef rtfw_device_status (*rtfw_device_cancel_fn)(
    void* instance,
    uint64_t submission_id);
typedef rtfw_device_status (*rtfw_device_get_health_fn)(
    void* instance,
    rtfw_device_health* health);
typedef rtfw_device_status (*rtfw_device_reset_fn)(void* instance);
typedef rtfw_device_status (*rtfw_device_shutdown_fn)(void* instance);

/*
 * The table and instance remain borrowed until shutdown returns. Every
 * function must be nonthrowing. submit and poll are bounded, nonblocking
 * operations; poll must never invoke runtime or host callbacks. submit and
 * poll may be invoked concurrently for one initialized backend instance and
 * the backend must synchronize its own fixed-capacity state without blocking.
 */
typedef struct rtfw_device_backend_api {
    uint32_t struct_size;
    uint32_t abi_version;
    void* instance;
    rtfw_device_get_capabilities_fn get_capabilities;
    rtfw_device_initialize_fn initialize;
    rtfw_device_register_buffer_fn register_buffer;
    rtfw_device_unregister_buffer_fn unregister_buffer;
    rtfw_device_submit_fn submit;
    rtfw_device_poll_fn poll;
    rtfw_device_cancel_fn cancel;
    rtfw_device_get_health_fn get_health;
    rtfw_device_reset_fn reset;
    rtfw_device_shutdown_fn shutdown;
    uint64_t reserved[8];
} rtfw_device_backend_api;

#ifdef __cplusplus
}
#endif

