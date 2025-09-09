#include <gtest/gtest.h>

#include <rt/plugin_manager.hpp>

#include <dlfcn.h>

TEST(PluginManager, LoadAndUnload) {
    auto plugin = rt::PluginManager::load(PLUGIN_PATH);
    EXPECT_STREQ(plugin.desc.name, "test_sample_plugin");
    auto is_init = reinterpret_cast<int (*)()>(dlsym(plugin.handle, "rt_plugin_is_initialized"));
    ASSERT_TRUE(is_init);
    EXPECT_EQ(is_init(), 1);
    rt::PluginManager::unload(plugin);
    EXPECT_EQ(plugin.handle, nullptr);
}

TEST(PluginManager, VersionMismatch) {
    EXPECT_THROW(rt::PluginManager::load(BAD_PLUGIN_PATH), std::runtime_error);
}
