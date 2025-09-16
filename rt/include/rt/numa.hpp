#pragma once

#include <cstddef>

#include <simcore/rt_memory.hpp>

#if defined(__linux__) && defined(SIM_USE_NUMA)
#include <numa.h>
#endif

namespace rt::numa {

inline bool available() noexcept {
#if defined(__linux__) && defined(SIM_USE_NUMA)
  return numa_available() != -1;
#else
  return false;
#endif
}

inline void bind_thread_to_node(int node) noexcept {
#if defined(__linux__) && defined(SIM_USE_NUMA)
  if (node >= 0 && available()) {
    numa_run_on_node(node);
    numa_set_preferred(node);
  }
#else
  (void)node;
#endif
}

using ThreadArena = FrameArena;

inline ThreadArena make_thread_arena(std::size_t size, int node) {
  return ThreadArena(size, 64, node);
}

} // namespace rt::numa

