#pragma once

#include <cstddef>
#include <algorithm>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

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

template <typename Fn>
void parallel_for_nodes(std::size_t total, const std::vector<int> &nodes,
                        Fn &&fn) {
  if (total == 0)
    return;

  if (nodes.empty()) {
    fn(0, total, -1);
    return;
  }

  const std::size_t threads = nodes.size();
  const std::size_t chunk = (total + threads - 1) / threads;

  auto fnPtr =
      std::make_shared<std::decay_t<Fn>>(std::forward<Fn>(fn));
  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (std::size_t t = 0; t < threads; ++t) {
    const std::size_t begin = t * chunk;
    if (begin >= total)
      break;
    const std::size_t end = std::min(total, begin + chunk);
    const int node = nodes[t % nodes.size()];
    workers.emplace_back([begin, end, node, fnPtr]() {
      if (node >= 0)
        bind_thread_to_node(node);
      (*fnPtr)(begin, end, node);
    });
  }
  for (auto &th : workers)
    th.join();
}

using ThreadArena = FrameArena;

inline ThreadArena make_thread_arena(std::size_t size, int node) {
  return ThreadArena(size, 64, node);
}

} // namespace rt::numa

