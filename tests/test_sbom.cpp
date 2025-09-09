#include <gtest/gtest.h>
#include <fstream>
#include <cstdlib>
#include <string>

TEST(SBOM, SubmoduleVerification) {
    std::string script = std::string(PROJECT_SOURCE_DIR) + "/tools/sbom.py";
    std::string expected = std::string(PROJECT_SOURCE_DIR) + "/tools/sbom_expected.json";
    std::string output = std::string(PROJECT_SOURCE_DIR) + "/sbom_test.json";
    std::string cmd = "python3 " + script + " " + expected + " > " + output;
    int rc = std::system(cmd.c_str());
    ASSERT_EQ(rc, 0);
    std::ifstream in(output);
    std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(contents.find("external/googletest"), std::string::npos);
    std::remove(output.c_str());
}
