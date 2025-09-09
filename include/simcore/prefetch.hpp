#pragma once
#include <cstddef>

namespace sim {

enum class PrefetchMode { Enabled, Disabled };

template <class T>
inline void prefetch(const T* ptr, std::size_t distance, PrefetchMode mode) {
    if (mode == PrefetchMode::Enabled) {
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(ptr + distance, 0, 3);
#endif
    }
}

} // namespace sim

