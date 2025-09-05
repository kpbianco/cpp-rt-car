// src/soa_simd.hpp
#pragma once
#include <cstddef>
#include <type_traits>

#if defined(__AVX2__)
  #include <immintrin.h>
#endif

namespace soa {

// Simple Structure-of-Arrays view for 3-vectors.
// We provide both mutable and const specializations.
template <typename T>
struct Vec3SoA {
    T* x;
    T* y;
    T* z;
};

template <typename T>
struct Vec3SoA<const T> {
    const T* x;
    const T* y;
    const T* z;
};

namespace detail {
#if defined(ENABLE_SIMD) && defined(__AVX2__)
// Precomputed masks for tail processing (1..3 elements)
alignas(32) static constexpr std::int64_t mask_lut[4][4] = {
    {0, 0, 0, 0},
    {-1, 0, 0, 0},
    {-1, -1, 0, 0},
    {-1, -1, -1, 0}
};
#endif
} // namespace detail

// p += a * v  (element-wise) for 3 channels (x,y,z)
inline void axpy3(double a,
                  Vec3SoA<const double> v,
                  Vec3SoA<double>       p,
                  std::size_t n)
{
#if defined(ENABLE_SIMD) && defined(__AVX2__)
    const __m256d aa = _mm256_set1_pd(a);
    std::size_t i = 0;

    // Vectorized body in 4-wide chunks
    for (; i + 4 <= n; i += 4) {
        // x
        __m256d px = _mm256_loadu_pd(p.x + i);
        __m256d vx = _mm256_loadu_pd(v.x + i);
        px = _mm256_fmadd_pd(aa, vx, px);
        _mm256_storeu_pd(p.x + i, px);

        // y
        __m256d py = _mm256_loadu_pd(p.y + i);
        __m256d vy = _mm256_loadu_pd(v.y + i);
        py = _mm256_fmadd_pd(aa, vy, py);
        _mm256_storeu_pd(p.y + i, py);

        // z
        __m256d pz = _mm256_loadu_pd(p.z + i);
        __m256d vz = _mm256_loadu_pd(v.z + i);
        pz = _mm256_fmadd_pd(aa, vz, pz);
        _mm256_storeu_pd(p.z + i, pz);
    }

    // Vectorized tail using masks (no scalar loop)
    std::size_t rem = n - i;
    if (rem) {
        const __m256i mask = _mm256_load_si256(reinterpret_cast<const __m256i*>(detail::mask_lut[rem]));

        __m256d px = _mm256_maskload_pd(p.x + i, mask);
        __m256d vx = _mm256_maskload_pd(v.x + i, mask);
        px = _mm256_fmadd_pd(aa, vx, px);
        _mm256_maskstore_pd(p.x + i, mask, px);

        __m256d py = _mm256_maskload_pd(p.y + i, mask);
        __m256d vy = _mm256_maskload_pd(v.y + i, mask);
        py = _mm256_fmadd_pd(aa, vy, py);
        _mm256_maskstore_pd(p.y + i, mask, py);

        __m256d pz = _mm256_maskload_pd(p.z + i, mask);
        __m256d vz = _mm256_maskload_pd(v.z + i, mask);
        pz = _mm256_fmadd_pd(aa, vz, pz);
        _mm256_maskstore_pd(p.z + i, mask, pz);
    }
#else
    // Portable scalar fallback (API is still available)
    for (std::size_t i = 0; i < n; ++i) {
        p.x[i] += a * v.x[i];
        p.y[i] += a * v.y[i];
        p.z[i] += a * v.z[i];
    }
#endif
}

} // namespace soa

