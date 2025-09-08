#pragma once

#include <stdint.h>

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
    return (cabi_version_t){1u, 0u};
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

#ifdef __cplusplus
}
#endif

