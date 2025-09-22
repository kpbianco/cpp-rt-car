#include <gtest/gtest.h>

#include <rt/plugin_manager.hpp>

#if defined(_WIN32)
#include <windows.h>
#define GETSYM(h, sym) GetProcAddress((HMODULE)(h), (sym))
#else
#include <dlfcn.h>
#define GETSYM(h, sym) dlsym((h), (sym))
#endif

TEST(PluginManager, LoadAndUnload) {
    auto plugin = rt::PluginManager::load(PLUGIN_PATH);
    EXPECT_TRUE(plugin.is_loaded());
    EXPECT_STREQ(plugin.desc.name, "test_sample_plugin");
    auto is_init = reinterpret_cast<int (*)()>(GETSYM(plugin.handle, "rt_plugin_is_initialized"));
    ASSERT_TRUE(is_init);
    EXPECT_EQ(is_init(), 1);
    rt::PluginManager::unload(plugin);
    EXPECT_FALSE(plugin.is_loaded());
}

TEST(PluginManager, VersionMismatch) {
    EXPECT_THROW(rt::PluginManager::load(BAD_PLUGIN_PATH), std::runtime_error);
}
