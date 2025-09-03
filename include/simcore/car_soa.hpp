#pragma once
#include <vector>
#include <cstddef>
#include <cassert>

/* -----------------------------------------------------------------
   Minimal struct‑of‑arrays for our dummy “car” example.
   Extend with more fields later (acc, steering, …).
   ----------------------------------------------------------------- */
struct CarSoA
{
    std::vector<double> pos;   // metres
    std::vector<double> vel;   // m/s

    explicit CarSoA(std::size_t n = 0)          { resize(n); }

    void         resize(std::size_t n)          { pos.resize(n); vel.resize(n); }
    std::size_t  size()   const                 { return pos.size(); }

    // helper accessors
    double  position(std::size_t i) const       { return pos[i]; }
    double  velocity(std::size_t i) const       { return vel[i]; }
    void    set(std::size_t i, double p, double v)
    {
        pos[i] = p;  vel[i] = v;
    }
};
