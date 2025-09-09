#include <gtest/gtest.h>
#include <simcore/minidump.hpp>
#include <fstream>
#include <vector>
#include <string>

TEST(Minidump, WritesSymbolPaths) {
#ifndef _WIN32
    std::string path = "minidump_symbols.txt";
    std::vector<std::string> symbols = {"/tmp/private1.pdb", "/tmp/private2.pdb"};
    write_minidump(path, symbols);
    std::ifstream in(path);
    std::string first;
    std::getline(in, first);
    EXPECT_NE(first.find("SYMBOL_PATH"), std::string::npos);
    std::remove(path.c_str());
#else
    GTEST_SKIP() << "Minidump symbol paths unsupported on Windows";
#endif
}
