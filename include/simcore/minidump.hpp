#pragma once
#include <string>
#include <fstream>
#include <vector>
#if defined(__unix__) || defined(__APPLE__)
#include <execinfo.h>
#include <cstdlib>
#endif

inline void write_minidump(const std::string& path,
                           const std::vector<std::string>& symbol_paths = {})
{
    std::ofstream out(path);
    if (!out) return;
    for (const auto& s : symbol_paths) {
        out << "SYMBOL_PATH " << s << '\n';
    }
#if defined(__unix__) || defined(__APPLE__)
    constexpr int MAX = 64;
    void* addrs[MAX];
    int n = ::backtrace(addrs, MAX);
    char** syms = ::backtrace_symbols(addrs, n);
    for (int i=0;i<n;++i) {
        out << syms[i] << '\n';
    }
    std::free(syms);
#else
    out << "minidump unsupported\n";
#endif
}
