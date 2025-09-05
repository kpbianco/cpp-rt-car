#pragma once
#include <vector>
#include <cstddef>
#include <cassert>
#include <algorithm>
#include "simcore/aligned_allocator.hpp"

// Generic sparse-set container with cacheline aligned dense storage
// and tail padding to avoid cacheline straddling.
template <typename T>
class SparseSet {
public:
    using value_type = T;
    static constexpr std::size_t CacheLine = 64;
    static constexpr std::size_t TailPad = CacheLine / sizeof(T);

    SparseSet() { ensure_capacity(); }

    bool has(std::size_t id) const {
        return id < sparse_.size() && sparse_[id] != 0;
    }

    T* get(std::size_t id) {
        std::size_t idxp1 = (id < sparse_.size()) ? sparse_[id] : 0;
        return idxp1 ? &dense_[idxp1 - 1] : &default_;
    }
    const T* get(std::size_t id) const {
        std::size_t idxp1 = (id < sparse_.size()) ? sparse_[id] : 0;
        return idxp1 ? &dense_[idxp1 - 1] : &default_;
    }

    void insert(std::size_t id, const T& value) {
        if (id >= sparse_.size())
            sparse_.resize(id + 1, 0);
        assert(!has(id) && "entity already present");
        ensure_capacity();
        std::size_t idx = dense_.size();
        dense_.push_back(value);
        entities_.push_back(id);
        sparse_[id] = idx + 1; // store index + 1
    }

    void remove(std::size_t id) {
        assert(has(id));
        std::size_t idx = sparse_[id] - 1;
        std::size_t last = dense_.size() - 1;
        if (idx != last) {
            dense_[idx] = dense_[last];
            std::size_t moved = entities_[last];
            entities_[idx] = moved;
            sparse_[moved] = idx + 1;
        }
        dense_.pop_back();
        entities_.pop_back();
        sparse_[id] = 0;
    }

    std::size_t size() const { return dense_.size(); }
    const std::vector<std::size_t>& entities() const { return entities_; }
    const T* data() const { return dense_.data(); }
    T* data() { return dense_.data(); }
    std::size_t capacity() const { return dense_.capacity(); }
    static constexpr std::size_t tail_padding() { return TailPad; }

private:
    void ensure_capacity() {
        if (dense_.capacity() - dense_.size() <= TailPad) {
            dense_.reserve(dense_.size() * 2 + TailPad);
        }
    }

    std::vector<T, AlignedAllocator<T, CacheLine>> dense_{};
    std::vector<std::size_t> sparse_{};
    std::vector<std::size_t> entities_{};
    T default_{};
};

// Grouped view over two sparse sets. Iterates over the smaller set and
// invokes the callback with matching components in a deterministic order.
template <typename Func, typename A, typename B>
void group_view(A& a, B& b, Func f) {
    if (a.size() > b.size()) {
        const auto& ents = b.entities();
        for (std::size_t i = 0; i < ents.size(); ++i) {
            std::size_t id = ents[i];
            if (a.has(id)) {
                f(id, *a.get(id), b.data()[i]);
            }
        }
    } else {
        const auto& ents = a.entities();
        for (std::size_t i = 0; i < ents.size(); ++i) {
            std::size_t id = ents[i];
            if (b.has(id)) {
                f(id, a.data()[i], *b.get(id));
            }
        }
    }
}

// Grouped view over three sparse sets.
template <typename Func, typename A, typename B, typename C>
void group_view(A& a, B& b, C& c, Func f) {
    group_view(a, b, [&](std::size_t id, auto& compA, auto& compB) {
        if (c.has(id))
            f(id, compA, compB, *c.get(id));
    });
}

