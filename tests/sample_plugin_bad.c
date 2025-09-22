#include <rt/plugin_api.h>

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

EXPORT rt_plugin_desc_t rt_plugin_desc = {
    RT_PLUGIN_API_VERSION_MAJOR + 1,
    0,
    "bad_plugin",
    "1.0",
};

EXPORT int rt_plugin_init(const rt_plugin_desc_t* desc) {
    (void)desc;
    return 0;
}

EXPORT void rt_plugin_shutdown(void) {}

EXPORT int rt_plugin_is_initialized(void) {
    return 0;
}
