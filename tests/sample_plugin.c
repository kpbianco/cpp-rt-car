#include <rt/plugin_api.h>

static int initialized = 0;

rt_plugin_desc rt_plugin_desc = {
    RT_PLUGIN_API_VERSION_MAJOR,
    RT_PLUGIN_API_VERSION_MINOR,
    "test_sample_plugin",
    "1.0"
};

int rt_plugin_init(const rt_plugin_desc* desc) {
    (void)desc;
    initialized = 1;
    return 0;
}

void rt_plugin_shutdown(void) {
    initialized = 0;
}

int rt_plugin_is_initialized(void) {
    return initialized;
}
