#pragma once
#include <cstddef>
#include <vector>
#include <cmath>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#include <simcore/prefetch.hpp>

namespace soa {

template <typename T, std::size_t TILE = 256>
struct Vec3AoSoA {
    static constexpr std::size_t tile_size = TILE;
    struct alignas(32) Tile { T x[TILE]; T y[TILE]; T z[TILE]; };
    Tile* tiles;
};

template <typename T, std::size_t TILE>
struct Vec3AoSoA<const T, TILE> {
    static constexpr std::size_t tile_size = TILE;
    using Tile = typename Vec3AoSoA<T, TILE>::Tile;
    const Tile* tiles;
};

template <std::size_t TILE>
inline void axpy3(double a,
                  Vec3AoSoA<const double, TILE> v,
                  Vec3AoSoA<double, TILE>       p,
                  std::size_t n) {
#if defined(ENABLE_SIMD) && defined(__AVX2__)
    static_assert(TILE % 4 == 0, "Tile size must be multiple of SIMD width");
    const __m256d aa = _mm256_set1_pd(a);
    const std::size_t full_tiles = n / TILE;
    for (std::size_t t = 0; t < full_tiles; ++t) {
        double* px = p.tiles[t].x;
        double* py = p.tiles[t].y;
        double* pz = p.tiles[t].z;
        const double* vx = v.tiles[t].x;
        const double* vy = v.tiles[t].y;
        const double* vz = v.tiles[t].z;
        for (std::size_t j = 0; j < TILE; j += 4) {
            sim::prefetch(px + j, 16, n, sim::PrefetchMode::Enabled);
            sim::prefetch(vx + j, 16, n, sim::PrefetchMode::Enabled);
            sim::prefetch(vy + j, 16, n, sim::PrefetchMode::Enabled);
            sim::prefetch(vz + j, 16, n, sim::PrefetchMode::Enabled);
            __m256d pxv = _mm256_loadu_pd(px + j);
            __m256d vxv = _mm256_loadu_pd(vx + j);
            pxv = _mm256_fmadd_pd(aa, vxv, pxv);
            _mm256_storeu_pd(px + j, pxv);

            __m256d pyv = _mm256_loadu_pd(py + j);
            __m256d vyv = _mm256_loadu_pd(vy + j);
            pyv = _mm256_fmadd_pd(aa, vyv, pyv);
            _mm256_storeu_pd(py + j, pyv);

            __m256d pzv = _mm256_loadu_pd(pz + j);
            __m256d vzv = _mm256_loadu_pd(vz + j);
            pzv = _mm256_fmadd_pd(aa, vzv, pzv);
            _mm256_storeu_pd(pz + j, pzv);
        }
    }
    std::size_t rem = n - full_tiles * TILE;
    if (rem) {
        std::size_t t = full_tiles;
        double* px = p.tiles[t].x;
        double* py = p.tiles[t].y;
        double* pz = p.tiles[t].z;
        const double* vx = v.tiles[t].x;
        const double* vy = v.tiles[t].y;
        const double* vz = v.tiles[t].z;
        std::size_t pad = (rem + 3) & ~std::size_t(3);
        for (std::size_t j = 0; j < pad; j += 4) {
            __m256d pxv = _mm256_loadu_pd(px + j);
            __m256d vxv = _mm256_loadu_pd(vx + j);
            pxv = _mm256_fmadd_pd(aa, vxv, pxv);
            _mm256_storeu_pd(px + j, pxv);

            __m256d pyv = _mm256_loadu_pd(py + j);
            __m256d vyv = _mm256_loadu_pd(vy + j);
            pyv = _mm256_fmadd_pd(aa, vyv, pyv);
            _mm256_storeu_pd(py + j, pyv);

            __m256d pzv = _mm256_loadu_pd(pz + j);
            __m256d vzv = _mm256_loadu_pd(vz + j);
            pzv = _mm256_fmadd_pd(aa, vzv, pzv);
            _mm256_storeu_pd(pz + j, pzv);
        }
    }
#else
    for (std::size_t i = 0; i < n; ++i) {
        std::size_t t = i / TILE;
        std::size_t j = i % TILE;
        p.tiles[t].x[j] = std::fma(a, v.tiles[t].x[j], p.tiles[t].x[j]);
        p.tiles[t].y[j] = std::fma(a, v.tiles[t].y[j], p.tiles[t].y[j]);
        p.tiles[t].z[j] = std::fma(a, v.tiles[t].z[j], p.tiles[t].z[j]);
    }
#endif
}

} // namespace soa

