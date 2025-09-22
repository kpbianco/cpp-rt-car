#pragma once

#include <stdint.h>

#if defined _WIN32 || defined __CYGWIN__
#  ifdef RTFW_BUILD
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

/*
 * Stable C ABI for embedding the simulation core.
 * Provides versioning and compile-time feature flag introspection.
 */

typedef struct {
    uint32_t major;
    uint32_t minor;
} cabi_version_t;

/* Feature flags exposed at compile time */
enum cabi_feature {
    CABI_FEATURE_LOG  = 1u << 0,
    CABI_FEATURE_PROF = 1u << 1,
};

typedef uint32_t cabi_feature_flags;

static inline cabi_version_t cabi_version(void) {
    cabi_version_t v = {1u, 0u};
    return v;
}

static inline cabi_feature_flags cabi_get_features(void) {
    cabi_feature_flags f = 0u;
#ifdef LOG_ENABLED
    f |= CABI_FEATURE_LOG;
#endif
#ifdef PROF_ENABLED
    f |= CABI_FEATURE_PROF;
#endif
    return f;
}

/*
 * Minimal runtime shim --------------------------------------------------
 */

typedef struct rtfw_handle rtfw_handle;

typedef enum rtfw_status {
    RTFW_STATUS_OK = 0,
    RTFW_STATUS_COMPLETE = 1,
    RTFW_STATUS_INVALID_ARGUMENT = -1,
    RTFW_STATUS_INTERNAL_ERROR = -2
} rtfw_status;

RTFW_API rtfw_handle* rtfw_create(double frame_budget_ms);
RTFW_API rtfw_status rtfw_step(rtfw_handle* handle);
RTFW_API void rtfw_destroy(rtfw_handle* handle);

#ifdef __cplusplus
}
#endif

