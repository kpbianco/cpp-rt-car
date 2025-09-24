#pragma once

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
#include <immintrin.h>
#elif defined(__aarch64__) || defined(__arm__) || defined(_M_ARM) || defined(_M_ARM64)
#  if defined(__has_include)
#    if __has_include(<arm_acle.h>)
#      include <arm_acle.h>
#    endif
#  endif
#endif

namespace rt {

inline void cpu_relax() noexcept {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__) || defined(_M_ARM) || defined(_M_ARM64)
#  if defined(_MSC_VER)
    __yield();
#  else
    __asm__ __volatile__("yield");
#  endif
#else
    // Fallback to no-op when no architecture-specific hint is available.
#endif
}

} // namespace rt

