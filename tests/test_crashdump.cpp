#include <gtest/gtest.h>
#include <rt/crashdump.hpp>
#include <fstream>
#include <thread>
#include <chrono>

TEST(CrashDump, WritesBacktrace) {
    const char* path = "crashdump_test.txt";
    rt::write_crashdump_async(path);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    EXPECT_FALSE(line.empty());
    std::remove(path);
}

