#include <rt/plugin_manager.hpp>

#include <dlfcn.h>
#include <stdexcept>

namespace rt {

PluginHandle PluginManager::load(const std::string& path) {
    void* handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle) {
        throw std::runtime_error(dlerror());
    }

    auto* desc = reinterpret_cast<rt_plugin_desc*>(dlsym(handle, "rt_plugin_desc"));
    if (!desc) {
        dlclose(handle);
        throw std::runtime_error("missing rt_plugin_desc symbol");
    }

    if (desc->abi_major != RT_PLUGIN_API_VERSION_MAJOR ||
        desc->abi_minor > RT_PLUGIN_API_VERSION_MINOR) {
        dlclose(handle);
        throw std::runtime_error("incompatible plugin ABI version");
    }

    auto init = reinterpret_cast<rt_plugin_init_fn>(dlsym(handle, "rt_plugin_init"));
    auto shutdown = reinterpret_cast<rt_plugin_shutdown_fn>(dlsym(handle, "rt_plugin_shutdown"));
    if (!init || !shutdown) {
        dlclose(handle);
        throw std::runtime_error("missing plugin entry points");
    }

    if (init(desc) != 0) {
        dlclose(handle);
        throw std::runtime_error("plugin initialization failed");
    }

    PluginHandle plugin{handle, *desc, shutdown};
    return plugin;
}

void PluginManager::unload(PluginHandle& plugin) {
    if (plugin.handle) {
        if (plugin.shutdown) {
            plugin.shutdown();
        }
        dlclose(plugin.handle);
        plugin.handle = nullptr;
        plugin.shutdown = nullptr;
    }
}

} // namespace rt

