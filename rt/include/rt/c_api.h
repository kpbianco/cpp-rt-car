#pragma once

#include <stdint.h>
#include <rtfw/version.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_VERSION_MAJOR RTFW_VERSION_MAJOR
#define RT_VERSION_MINOR RTFW_VERSION_MINOR

typedef struct {
    uint8_t jobs;
    uint8_t time;
    uint8_t memory;
} rt_capabilities_c;

// Query version
uint32_t rt_version_major(void);
uint32_t rt_version_minor(void);

// Query runtime capabilities
rt_capabilities_c rt_query_capabilities(void);

#ifdef __cplusplus
}
#endif
