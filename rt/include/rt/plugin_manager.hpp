#pragma once

#include <string>

#include <rt/plugin_api.h>

namespace rt {

struct PluginHandle {
    void* handle{nullptr};
    rt_plugin_desc_t desc{};
    rt_plugin_shutdown_fn shutdown{nullptr};

    [[nodiscard]] bool is_loaded() const noexcept { return handle != nullptr; }

    void reset() noexcept {
        handle = nullptr;
        shutdown = nullptr;
        desc = {};
    }
};

class PluginManager {
public:
    static PluginHandle load(const std::string& path);
    static void unload(PluginHandle& plugin);
};

} // namespace rt

