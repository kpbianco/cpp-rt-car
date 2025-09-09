#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>

#include "core/config_hot_reload.hpp"

TEST(ConfigHotReload, ReloadsUpdatedFile) {
  const std::string path = "test_config.txt";
  {
    std::ofstream out(path);
    out << "1 0";
  }

  core::ConfigHotReloader loader(path);
  ASSERT_TRUE(loader.load());
  EXPECT_EQ(loader.get()->minor, 0u);

  // Ensure filesystem timestamp resolution doesn't cause spurious failures
  std::this_thread::sleep_for(std::chrono::seconds(1));

  // Update configuration on disk
  {
    std::ofstream out(path);
    out << "1 1";
  }

  ASSERT_TRUE(loader.hot_reload());
  EXPECT_EQ(loader.get()->minor, 1u);

  std::filesystem::remove(path);
}
