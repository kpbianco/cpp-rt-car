#pragma once
#include <cstddef>

namespace sim {

enum class PrefetchMode { Enabled, Disabled };

// Only issue prefetch instructions when the working set is large enough to
// benefit from them. Small ranges tend to fit in cache and manual prefetching
// can actually hurt performance. The heuristic below can be tuned based on
// profiling, but keeps the interface simple for callers.
constexpr std::size_t PREFETCH_MIN_N = 1024; // elements

template <class T>
inline void prefetch(const T* ptr,
                     std::size_t   distance,
                     std::size_t   n,
                     PrefetchMode  mode) {
    if (mode == PrefetchMode::Enabled && n >= PREFETCH_MIN_N) {
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(ptr + distance, 0, 3);
#else
        (void)ptr; (void)distance;
#endif
    } else {
        (void)ptr; (void)distance;
    }
}

} // namespace sim

