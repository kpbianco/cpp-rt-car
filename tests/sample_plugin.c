#include <rt/plugin_api.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

static int g_initialized = 0;

EXPORT rt_plugin_desc_t rt_plugin_desc = {
    RT_PLUGIN_API_VERSION_MAJOR,
    RT_PLUGIN_API_VERSION_MINOR,
    "test_sample_plugin",
    "1.0"
};

EXPORT int rt_plugin_init(const rt_plugin_desc_t* desc) {
    (void)desc;
    g_initialized = 1;
    return 0;
}

EXPORT void rt_plugin_shutdown(void) {
    g_initialized = 0;
}

EXPORT int rt_plugin_is_initialized(void) {
    return g_initialized;
}
