#pragma once
#include <cfenv>
#include <cstdint>
#include <cmath>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

namespace rt {

// Configure floating point environment for determinism.
// Sets rounding mode to FE_TONEAREST and enables flush-to-zero
// and denormals-are-zero on x86 targets. Intended to be called
// once per thread at startup.
inline void init_fp_env() {
    std::fesetround(FE_TONEAREST);
#if defined(__SSE__)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

inline bool &use_fma_flag() {
    static bool flag = true;
    return flag;
}

inline void set_use_fma(bool on) { use_fma_flag() = on; }
inline bool use_fma() { return use_fma_flag(); }

// Wrapper for fused multiply-add with a runtime gate. When the
// gate is disabled the operation is performed as a separate
// multiply and add which mirrors platforms lacking hardware FMA.
inline double fma(double a, double b, double c) {
    if (use_fma_flag())
        return std::fma(a, b, c);
    return a * b + c;
}

} // namespace rt

