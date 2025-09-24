#pragma once
#include "rt_memory.hpp"
#include <rt/numa.hpp>
#include <cassert>
#ifndef NDEBUG
#  include <cstdio>
#endif

#ifndef RTFW_DEBUG_ASSERT
#  ifdef NDEBUG
#    define RTFW_DEBUG_ASSERT(...) ((void)0)
#  else
namespace simcore::detail {
inline void debug_assert(bool expr) { assert(expr); }
inline void debug_assert(bool expr, const char *msg) {
  if (!expr) {
    std::fputs(msg, stderr);
    std::fputc('\n', stderr);
  }
  assert(expr);
}
} // namespace simcore::detail
#    define RTFW_DEBUG_ASSERT(...) ::simcore::detail::debug_assert(__VA_ARGS__)
#  endif
#endif

inline thread_local bool tls_arena_bound = false;

inline bool isCurrentThreadArenaBound() noexcept { return tls_arena_bound; }

using FrameArena = rt::FrameArena;
using FrameArenaPool = rt::FrameArenaPool;
using ThreadArena = rt::numa::ThreadArena;
