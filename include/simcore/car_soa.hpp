#pragma once
#include <vector>
#include <cstddef>
#include <cassert>

#include "aligned_allocator.hpp"

/* -----------------------------------------------------------------
   Minimal struct‑of‑arrays for our dummy “car” example.
   Extend with more fields later (acc, steering, …).
   ----------------------------------------------------------------- */
// Component arrays are 64-byte aligned and padded to a multiple of SIMD
// width (4 doubles) to keep vectorized loops simple. We also allocate in
// blocks (AoSoA style) to improve TLB/cache locality when the entity count
// grows large.
struct CarSoA
{
    using AlignedDVec = std::vector<double, AlignedAllocator<double,64>>;

    AlignedDVec pos;   // metres
    AlignedDVec vel;   // m/s

    std::size_t size_    = 0;   // logical count
    std::size_t padded_  = 0;   // padded storage count

    explicit CarSoA(std::size_t n = 0)          { resize(n); }

    static std::size_t pad(std::size_t n) {
        const std::size_t simdWidth = 4; // AVX2: 4 doubles
        return (n + simdWidth - 1) & ~(simdWidth - 1);
    }

    void resize(std::size_t n) {
        size_ = n;
        padded_ = pad(n);
        pos.resize(padded_);
        vel.resize(padded_);
    }

    std::size_t size() const { return size_; }

    // helper accessors
    double  position(std::size_t i) const       { return pos[i]; }
    double  velocity(std::size_t i) const       { return vel[i]; }
    void    set(std::size_t i, double p, double v)
    {
        pos[i] = p;  vel[i] = v;
    }
};
