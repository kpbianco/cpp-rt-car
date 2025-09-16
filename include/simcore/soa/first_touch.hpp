#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

#include <rt/numa.hpp>
#include <simcore/soa/aosoa.hpp>
#include <simcore/soa/soa_simd.hpp>

namespace soa {

template <typename T>
void init_vec3_soa(Vec3SoA<T> soa, std::size_t n,
                   const std::vector<int> &nodes, T value = T{}) {
  static_assert(!std::is_const_v<T>, "Vec3SoA initializer requires mutable T");
  if (!soa.x || !soa.y || !soa.z || n == 0)
    return;

  auto work = [soa, value](std::size_t begin, std::size_t end, int) {
    for (std::size_t i = begin; i < end; ++i) {
      soa.x[i] = value;
      soa.y[i] = value;
      soa.z[i] = value;
    }
  };
  rt::numa::parallel_for_nodes(n, nodes, std::move(work));
}

template <typename T>
void init_vec3_soa(Vec3SoA<T> soa, std::size_t n, T value = T{}) {
  init_vec3_soa(soa, n, std::vector<int>{}, value);
}

template <typename T, std::size_t TILE>
void init_vec3_aosoa(Vec3AoSoA<T, TILE> aosoa, std::size_t elements,
                      const std::vector<int> &nodes, T value = T{}) {
  static_assert(!std::is_const_v<T>,
                "Vec3AoSoA initializer requires mutable T");
  if (!aosoa.tiles || elements == 0)
    return;

  const std::size_t tileCount = (elements + TILE - 1) / TILE;
  if (tileCount == 0)
    return;
  const std::size_t totalSlots = tileCount * TILE;

  auto work = [aosoa, value](std::size_t begin, std::size_t end, int) {
    for (std::size_t idx = begin; idx < end; ++idx) {
      const std::size_t tile = idx / TILE;
      const std::size_t lane = idx % TILE;
      aosoa.tiles[tile].x[lane] = value;
      aosoa.tiles[tile].y[lane] = value;
      aosoa.tiles[tile].z[lane] = value;
    }
  };
  rt::numa::parallel_for_nodes(totalSlots, nodes, std::move(work));
}

template <typename T, std::size_t TILE>
void init_vec3_aosoa(Vec3AoSoA<T, TILE> aosoa, std::size_t elements,
                      T value = T{}) {
  init_vec3_aosoa(aosoa, elements, std::vector<int>{}, value);
}

} // namespace soa

