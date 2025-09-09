#include <gtest/gtest.h>
#include <fstream>
#include <cstdlib>
#include <string>

TEST(ReproBuild, StoresArtifactWithHash) {
    std::string script = std::string(PROJECT_SOURCE_DIR) + "/tools/store_repro_build.py";
    std::string artifact = std::string(PROJECT_SOURCE_DIR) + "/CMakeLists.txt";
    std::string dest = std::string(PROJECT_SOURCE_DIR) + "/perf_artifacts_test";
    std::string cmd = "python3 " + script + " " + artifact + " " + dest;
    int rc = std::system(cmd.c_str());
    ASSERT_EQ(rc, 0);
    std::ifstream in(dest + "/build.json");
    ASSERT_TRUE(in.good());
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find("CMakeLists.txt"), std::string::npos);
    std::remove(std::string(dest + "/build.json").c_str());
    std::remove(std::string(dest + "/CMakeLists.txt").c_str());
    rmdir(dest.c_str());
}
