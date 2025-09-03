#pragma once
#include <vector>
#include <cstddef>
#include <cassert>
#include <utility>

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
