#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_PLUGIN_API_VERSION_MAJOR 1
#define RT_PLUGIN_API_VERSION_MINOR 0

typedef struct {
    uint32_t abi_major;
    uint32_t abi_minor;
    const char* name;
    const char* version;
} rt_plugin_desc;

typedef int (*rt_plugin_init_fn)(const rt_plugin_desc* desc);
typedef void (*rt_plugin_shutdown_fn)(void);

#ifdef __cplusplus
}
#endif

