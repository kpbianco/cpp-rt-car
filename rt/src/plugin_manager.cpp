#include <rt/plugin_manager.hpp>

#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace rt {

PluginHandle PluginManager::load(const std::string& path) {
#if defined(_WIN32)
    HMODULE handle = LoadLibraryA(path.c_str());
    if (!handle) {
        throw std::runtime_error("LoadLibrary failed");
    }

    auto* desc = reinterpret_cast<rt_plugin_desc_t*>(GetProcAddress(handle, "rt_plugin_desc"));
    if (!desc) {
        FreeLibrary(handle);
        throw std::runtime_error("missing rt_plugin_desc symbol");
    }

    if (desc->abi_major != RT_PLUGIN_API_VERSION_MAJOR ||
        desc->abi_minor > RT_PLUGIN_API_VERSION_MINOR) {
        FreeLibrary(handle);
        throw std::runtime_error("incompatible plugin ABI version");
    }

    auto init = reinterpret_cast<rt_plugin_init_fn>(GetProcAddress(handle, "rt_plugin_init"));
    auto shutdown = reinterpret_cast<rt_plugin_shutdown_fn>(GetProcAddress(handle, "rt_plugin_shutdown"));
    if (!init || !shutdown) {
        FreeLibrary(handle);
        throw std::runtime_error("missing plugin entry points");
    }

    if (init(desc) != 0) {
        FreeLibrary(handle);
        throw std::runtime_error("plugin initialization failed");
    }

    PluginHandle plugin{handle, *desc, shutdown};
    return plugin;
#else
    void* handle = dlopen(path.c_str(), RTLD_NOW);
    if (!handle) {
        throw std::runtime_error(dlerror());
    }

    auto* desc = reinterpret_cast<rt_plugin_desc_t*>(dlsym(handle, "rt_plugin_desc"));
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
#endif
}

void PluginManager::unload(PluginHandle& plugin) {
#if defined(_WIN32)
    if (plugin.handle) {
        if (plugin.shutdown) {
            plugin.shutdown();
        }
        FreeLibrary(static_cast<HMODULE>(plugin.handle));
        plugin.handle = nullptr;
        plugin.shutdown = nullptr;
    }
#else
    if (plugin.handle) {
        if (plugin.shutdown) {
            plugin.shutdown();
        }
        dlclose(plugin.handle);
        plugin.handle = nullptr;
        plugin.shutdown = nullptr;
    }
#endif
}

} // namespace rt

