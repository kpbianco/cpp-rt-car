#pragma once
#include <cstddef>
#include <cstdint>

namespace soa {

// Strided span representing a channel inside an AoS buffer.
template <typename T>
struct StridedSpan {
    T* ptr;
    std::size_t stride; // bytes
    T& operator[](std::size_t i) const {
        return *reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(ptr) + i * stride);
    }
};

// View over legacy AoS arrays exposing SoA-like access.
template <typename T>
struct Vec3AoSView {
    StridedSpan<T> x;
    StridedSpan<T> y;
    StridedSpan<T> z;

    template <typename Struct>
    static Vec3AoSView from(Struct* data,
                            T Struct::*px,
                            T Struct::*py,
                            T Struct::*pz) {
        return {
            { &(data->*px), sizeof(Struct) },
            { &(data->*py), sizeof(Struct) },
            { &(data->*pz), sizeof(Struct) }
        };
    }
};

// p += s * v for AoS views (scalar implementation)
template <typename T>
inline void axpy3(Vec3AoSView<T> v,
                  Vec3AoSView<T> p,
                  T s,
                  std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        p.x[i] += s * v.x[i];
        p.y[i] += s * v.y[i];
        p.z[i] += s * v.z[i];
    }
}

} // namespace soa

