#pragma once

/*
 * Compatibility include for the pre-M1 header path. New integrations should
 * include <rt/c_api.h>. Stable ABI v8 is documented in docs/c_abi.md.
 */
#include <rt/c_api.h>

typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} cabi_version_t;

enum cabi_feature {
    CABI_FEATURE_LOG  = 1u << 0,
    CABI_FEATURE_PROF = 1u << 1,
};

typedef uint32_t cabi_feature_flags;

static inline cabi_version_t cabi_version(void) {
    cabi_version_t version = {
        RTFW_VERSION_MAJOR,
        RTFW_VERSION_MINOR,
        RTFW_VERSION_PATCH
    };
    return version;
}

static inline cabi_feature_flags cabi_get_features(void) {
    cabi_feature_flags features = 0u;
#ifdef LOG_ENABLED
    features |= CABI_FEATURE_LOG;
#endif
#ifdef PROF_ENABLED
    features |= CABI_FEATURE_PROF;
#endif
    return features;
}
