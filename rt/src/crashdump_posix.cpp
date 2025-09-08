#include "rt/crashdump.hpp"

#include <execinfo.h>
#include <fstream>
#include <thread>
#include <vector>
#include <cstdlib>

namespace rt {

void write_crashdump_async(const std::string &path) {
    std::thread([path] {
        void *buffer[64];
        int n = ::backtrace(buffer, 64);
        std::ofstream out(path);
        if (!out) return;
        char **symbols = ::backtrace_symbols(buffer, n);
        for (int i = 0; i < n; ++i) {
            out << symbols[i] << '\n';
        }
        std::free(symbols);
    }).detach();
}

} // namespace rt

