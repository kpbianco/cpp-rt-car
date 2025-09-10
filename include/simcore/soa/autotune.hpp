#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include <simcore/soa/aosoa.hpp>

#if defined(__x86_64__) || defined(_M_X64) || defined(_M_IX86)
#    if defined(_MSC_VER)
#        include <intrin.h>
#    else
#        include <x86intrin.h>
#    endif
#endif

namespace soa {

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
inline std::uint64_t read_cycles() { return __rdtsc(); }
#else
inline std::uint64_t read_cycles() { return 0; }
#endif

template <std::size_t TILE>
inline std::uint64_t measure_axpy3(std::size_t n) {
    std::size_t tiles = (n + TILE - 1) / TILE;
    std::vector<typename Vec3AoSoA<double, TILE>::Tile> pv(tiles), vv(tiles);
    Vec3AoSoA<double, TILE> p{pv.data()};
    Vec3AoSoA<const double, TILE> v{vv.data()};
    auto start = read_cycles();
    axpy3<TILE>(1.0, v, p, n);
    auto end = read_cycles();
    return end - start;
}

inline std::size_t autotune_block_size(std::size_t n) {
    std::array<std::size_t,4> candidates{64,128,256,512};
    std::size_t best = candidates[0];
    std::uint64_t best_cycles = std::numeric_limits<std::uint64_t>::max();
    for (auto s : candidates) {
        std::uint64_t cycles = 0;
        switch (s) {
            case 64:  cycles = measure_axpy3<64>(n);  break;
            case 128: cycles = measure_axpy3<128>(n); break;
            case 256: cycles = measure_axpy3<256>(n); break;
            case 512: cycles = measure_axpy3<512>(n); break;
        }
        if (cycles < best_cycles) {
            best_cycles = cycles;
            best = s;
        }
    }
    return best;
}

} // namespace soa
