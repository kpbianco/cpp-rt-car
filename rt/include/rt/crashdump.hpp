#pragma once
#include <string>

namespace rt {

// Writes a simple stack backtrace to the specified path asynchronously.
// Intended for use in crash handlers where blocking is undesirable.
void write_crashdump_async(const std::string &path);

} // namespace rt

