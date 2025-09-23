#pragma once
#include "rt_memory.hpp"
#include <rt/numa.hpp>
#include <cassert>

#ifndef RTFW_DEBUG_ASSERT
#  ifdef NDEBUG
#    define RTFW_DEBUG_ASSERT(expr) ((void)0)
#  else
#    define RTFW_DEBUG_ASSERT(expr) assert(expr)
#  endif
#endif

inline thread_local bool tls_arena_bound = false;

inline bool isCurrentThreadArenaBound() noexcept { return tls_arena_bound; }

using FrameArena = rt::FrameArena;
using FrameArenaPool = rt::FrameArenaPool;
using ThreadArena = rt::numa::ThreadArena;
