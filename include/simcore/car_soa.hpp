#pragma once
#include <vector>
#include <cstddef>
#include <cassert>
#include <utility>
#include "aligned_allocator.hpp"

/* -----------------------------------------------------------------
   Minimal struct‑of‑arrays for our dummy “car” example.
   Extend with more fields later (acc, steering, …).
   ----------------------------------------------------------------- */

struct CarSoA
{
    static constexpr std::size_t block_size = 128;

    struct alignas(64) Block
    {
        alignas(64) double pos[block_size];   // metres
        alignas(64) double vel[block_size];   // m/s
    };

    std::vector<Block> m_blocks;
    std::size_t        m_size = 0;   // number of valid elements

    explicit CarSoA(std::size_t n = 0)          { resize(n); }

    void resize(std::size_t n)
    {
        m_size = n;
        std::size_t nblocks = (n + block_size - 1) / block_size;
        m_blocks.resize(nblocks);
    }

    std::size_t size() const                   { return m_size; }

    // helper accessors
    double& position(std::size_t i)
    {
        auto [b, o] = split(i); return m_blocks[b].pos[o];
    }
    double  position(std::size_t i) const
    {
        auto [b, o] = split(i); return m_blocks[b].pos[o];
    }
    double& velocity(std::size_t i)
    {
        auto [b, o] = split(i); return m_blocks[b].vel[o];
    }
    double  velocity(std::size_t i) const
    {
        auto [b, o] = split(i); return m_blocks[b].vel[o];
    }
    void    set(std::size_t i, double p, double v)
    {
        auto [b, o] = split(i);
        m_blocks[b].pos[o] = p;  m_blocks[b].vel[o] = v;
    }

    std::vector<Block>&       blocks()       { return m_blocks; }
    const std::vector<Block>& blocks() const { return m_blocks; }

private:
    static std::pair<std::size_t, std::size_t> split(std::size_t i)
    {
        return { i / block_size, i % block_size };
    }
};

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

