#include <rt/plugin_api.h>

rt_plugin_desc rt_plugin_desc = {
    RT_PLUGIN_API_VERSION_MAJOR + 1,
    0,
    "bad_plugin",
    "1.0"
};

int rt_plugin_init(const rt_plugin_desc* desc) {
    (void)desc;
    return 0;
}

void rt_plugin_shutdown(void) {}
