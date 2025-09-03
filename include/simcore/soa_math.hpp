#pragma once
#include <cstddef>
#include "simd.hpp"

namespace soa {

// Lightweight SoA triple (non-owning).
template<typename T>
struct Vec3SoA {
    T* x; T* y; T* z;
};

// pos += vel * s
template<typename T>
inline void axpy3(Vec3SoA<T> pos,
                  const Vec3SoA<const T> vel,
                  T s, std::size_t n)
{
    simd::axpy(pos.x, vel.x, s, n);
    simd::axpy(pos.y, vel.y, s, n);
    simd::axpy(pos.z, vel.z, s, n);
}

// dst = a*x + b*y
template<typename T>
inline void axpby3(Vec3SoA<T> dst,
                   const Vec3SoA<const T> x, T a,
                   const Vec3SoA<const T> y, T b,
                   std::size_t n)
{
    simd::axpbz(dst.x, x.x, a, y.x, b, n);
    simd::axpbz(dst.y, x.y, a, y.y, b, n);
    simd::axpbz(dst.z, x.z, a, y.z, b, n);
}

} // namespace soa
