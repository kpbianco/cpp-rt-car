#pragma once

#include <cassert>
#include <cstdint>

#ifndef RTFW_ENFORCE_NO_RAW_THREADS
#  if !defined(NDEBUG)
#    define RTFW_ENFORCE_NO_RAW_THREADS 1
#  else
#    define RTFW_ENFORCE_NO_RAW_THREADS 0
#  endif
#endif

namespace simcore::debug {

#if RTFW_ENFORCE_NO_RAW_THREADS

inline thread_local std::uint32_t phase_scope_depth = 0;

class PhaseScope {
public:
  PhaseScope() noexcept { phase_scope_depth += 1; }
  PhaseScope(const PhaseScope &) = delete;
  PhaseScope &operator=(const PhaseScope &) = delete;
  ~PhaseScope() {
    assert(phase_scope_depth > 0);
    phase_scope_depth -= 1;
  }
};

inline bool in_phase_scope() noexcept { return phase_scope_depth != 0; }

inline void assert_thread_creation_allowed() {
  assert(!in_phase_scope() &&
         "SimCore phases must not spawn OS threads; use WorkerPool::submit");
}

#else

class PhaseScope {
public:
  PhaseScope() noexcept = default;
  PhaseScope(const PhaseScope &) = delete;
  PhaseScope &operator=(const PhaseScope &) = delete;
};

inline bool in_phase_scope() noexcept { return false; }

inline void assert_thread_creation_allowed() {}

#endif

} // namespace simcore::debug

