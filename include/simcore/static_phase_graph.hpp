#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace simcore {

// Compile-time validated phase graph with runtime overlays and
// automatic read/write set tracking to synthesize barriers.

template <std::size_t N, std::array<std::array<bool, N>, N> Edges>
class StaticPhaseGraph {
  struct RW {
    const void *ptr;
    std::size_t bytes;
  };

  struct Phase {
    std::function<void()> task;
    std::vector<RW> reads;
    std::vector<RW> writes;
  };

  static constexpr auto edges_ = Edges;

  std::array<Phase, N> phases_{};
  std::vector<std::pair<std::size_t, std::size_t>> overlays_;

  static constexpr bool isAcyclic() {
    std::array<int, N> indegree{};
    for (std::size_t i = 0; i < N; ++i)
      for (std::size_t j = 0; j < N; ++j)
        if (edges_[i][j])
          ++indegree[j];

    std::array<bool, N> removed{};
    for (std::size_t k = 0; k < N; ++k) {
      std::size_t i = 0;
      for (; i < N; ++i)
        if (!removed[i] && indegree[i] == 0)
          break;
      if (i == N)
        return false; // cycle
      removed[i] = true;
      for (std::size_t j = 0; j < N; ++j)
        if (edges_[i][j])
          --indegree[j];
    }
    return true;
  }

  static_assert(isAcyclic(), "Phase graph must be acyclic");

  static bool overlap(const RW &a, const RW &b) {
    auto as = reinterpret_cast<std::uintptr_t>(a.ptr);
    auto ae = as + a.bytes;
    auto bs = reinterpret_cast<std::uintptr_t>(b.ptr);
    auto be = bs + b.bytes;
    return as < be && bs < ae;
  }

  static bool conflict(const Phase &a, const Phase &b) {
    for (const auto &w : a.writes) {
      for (const auto &r : b.reads)
        if (overlap(w, r))
          return true;
      for (const auto &w2 : b.writes)
        if (overlap(w, w2))
          return true;
    }
    for (const auto &r : a.reads)
      for (const auto &w : b.writes)
        if (overlap(r, w))
          return true;
    return false;
  }

public:
  StaticPhaseGraph() = default;

  template <class F> void setPhaseTask(std::size_t idx, F &&f) {
    phases_[idx].task = std::forward<F>(f);
  }

  void addOverlay(std::size_t from, std::size_t to) {
    overlays_.emplace_back(from, to);
  }

  template <class T>
  std::span<const T> read(std::size_t idx, std::span<const T> s) {
    phases_[idx].reads.push_back({s.data(), s.size() * sizeof(T)});
    return s;
  }

  template <class T> std::span<T> write(std::size_t idx, std::span<T> s) {
    phases_[idx].writes.push_back({s.data(), s.size() * sizeof(T)});
    return s;
  }

  void run() {
    auto edges = edges_;
    for (auto [a, b] : overlays_)
      edges[a][b] = true;
    for (std::size_t i = 0; i < N; ++i)
      for (std::size_t j = i + 1; j < N; ++j)
        if (conflict(phases_[i], phases_[j]))
          edges[i][j] = true;

    std::array<int, N> indegree{};
    for (std::size_t i = 0; i < N; ++i)
      for (std::size_t j = 0; j < N; ++j)
        if (edges[i][j])
          ++indegree[j];

    std::array<bool, N> done{};
    for (std::size_t step = 0; step < N; ++step) {
      std::size_t node = N;
      for (std::size_t i = 0; i < N; ++i)
        if (!done[i] && indegree[i] == 0) {
          node = i;
          break;
        }
      if (node == N)
        throw std::runtime_error("cycle in phase graph");
      done[node] = true;
      if (phases_[node].task)
        phases_[node].task();
      for (std::size_t j = 0; j < N; ++j)
        if (edges[node][j])
          --indegree[j];
    }
  }
};

} // namespace simcore
