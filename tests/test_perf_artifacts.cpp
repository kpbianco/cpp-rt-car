#include <gtest/gtest.h>
#include <fstream>
#include <cstdlib>
#include <string>
#include <filesystem>

TEST(ReproBuild, StoresArtifactWithHash) {
    std::string script = std::string(PROJECT_SOURCE_DIR) + "/tools/store_repro_build.py";
    std::string artifact = std::string(PROJECT_SOURCE_DIR) + "/CMakeLists.txt";
    std::string dest = std::string(PROJECT_SOURCE_DIR) + "/perf_artifacts_test";
    std::string cmd = "python3 " + script + " " + artifact + " " + dest;
    int rc = std::system(cmd.c_str());
    ASSERT_EQ(rc, 0);
    {
        std::ifstream in(dest + "/build.json");
        ASSERT_TRUE(in.good());
        std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        EXPECT_NE(contents.find("CMakeLists.txt"), std::string::npos);
    } // ensure file is closed before removal
    std::filesystem::remove(dest + "/build.json");
    std::filesystem::remove(dest + "/CMakeLists.txt");
    std::filesystem::remove_all(dest);
}
