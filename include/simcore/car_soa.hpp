#pragma once
#include <vector>
#include <cstddef>
#include <cassert>
#include "simcore/aligned_allocator.hpp"

/* -----------------------------------------------------------------
   Struct-of-arrays storage for the dummy "car" example.
   Now maintains sparse/dense mappings so systems can query only the
   components they need. Dense arrays are cacheline aligned with tail
   padding to avoid cross-line writes.
   ----------------------------------------------------------------- */
struct CarSoA
{
    static constexpr std::size_t CacheLine = 64;
    static constexpr std::size_t TailPad = CacheLine / sizeof(double);

    // dense component storage
    std::vector<double, AlignedAllocator<double, CacheLine>> pos;   // metres
    std::vector<double, AlignedAllocator<double, CacheLine>> vel;   // m/s
    // entity mappings
    std::vector<std::size_t> sparse;  // entity -> dense index + 1 (0 = missing)
    std::vector<std::size_t> dense;   // dense index -> entity id

    double defaultPos{0.0};
    double defaultVel{0.0};

    CarSoA() = default;
    explicit CarSoA(std::size_t n)
    {
        reserve(n);
        for (std::size_t i = 0; i < n; ++i)
            insert(i, 0.0, 0.0);
    }

    void reserve(std::size_t n)
    {
        pos.reserve(n + TailPad);
        vel.reserve(n + TailPad);
        dense.reserve(n);
        sparse.reserve(n);
    }

    std::size_t size() const { return pos.size(); }

    bool has(std::size_t id) const
    {
        return id < sparse.size() && sparse[id] != 0;
    }

    void insert(std::size_t id, double p, double v)
    {
        if (id >= sparse.size())
            sparse.resize(id + 1, 0);
        assert(sparse[id] == 0 && "entity already present");
        ensure_capacity();
        std::size_t idx = pos.size();
        pos.push_back(p);
        vel.push_back(v);
        dense.push_back(id);
        sparse[id] = idx + 1;               // store index+1 (0 reserved for missing)
    }

    void remove(std::size_t id)
    {
        assert(id < sparse.size());
        std::size_t idxp1 = sparse[id];
        assert(idxp1 != 0 && "entity missing");
        std::size_t idx = idxp1 - 1;
        std::size_t last = pos.size() - 1;
        if (idx != last)
        {
            pos[idx] = pos[last];
            vel[idx] = vel[last];
            std::size_t moved = dense[last];
            dense[idx] = moved;
            sparse[moved] = idx + 1;
        }
        pos.pop_back();
        vel.pop_back();
        dense.pop_back();
        sparse[id] = 0;
    }

    // Branchless lookup: return pointer to component or default value.
    double* lookup_position(std::size_t id)
    {
        std::size_t idxp1 = (id < sparse.size()) ? sparse[id] : 0;
        return idxp1 ? &pos[idxp1 - 1] : &defaultPos;
    }
    const double* lookup_position(std::size_t id) const
    {
        std::size_t idxp1 = (id < sparse.size()) ? sparse[id] : 0;
        return idxp1 ? &pos[idxp1 - 1] : &defaultPos;
    }
    double* lookup_velocity(std::size_t id)
    {
        std::size_t idxp1 = (id < sparse.size()) ? sparse[id] : 0;
        return idxp1 ? &vel[idxp1 - 1] : &defaultVel;
    }
    const double* lookup_velocity(std::size_t id) const
    {
        std::size_t idxp1 = (id < sparse.size()) ? sparse[id] : 0;
        return idxp1 ? &vel[idxp1 - 1] : &defaultVel;
    }

    template <typename Func>
    void for_each(Func f) {
        for (std::size_t i = 0; i < pos.size(); ++i)
            f(dense[i], pos[i], vel[i]);
    }

private:
    void ensure_capacity() {
        if (pos.capacity() - pos.size() <= TailPad)
            pos.reserve(pos.size() * 2 + TailPad);
        if (vel.capacity() - vel.size() <= TailPad)
            vel.reserve(vel.size() * 2 + TailPad);
    }
};

