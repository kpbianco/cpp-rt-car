#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <simcore/static_phase_graph.hpp>
#include <vector>

inline constexpr std::array<std::array<bool, 4>, 4> kEdges{
    {{{false, false, true, false}},
     {{false, false, true, false}},
     {{false, false, false, true}},
     {{false, false, false, false}}}};

inline constexpr std::array<std::array<bool, 2>, 2> kNoEdges{
    {{{false, false}}, {{false, false}}}};

TEST(StaticPhaseGraph, RespectsCompileTimeEdges) {
  using Graph = simcore::StaticPhaseGraph<4, kEdges>;
  Graph g;
  std::vector<int> order;
  g.setPhaseTask(0, [&] { order.push_back(0); });
  g.setPhaseTask(1, [&] { order.push_back(1); });
  g.setPhaseTask(2, [&] { order.push_back(2); });
  g.setPhaseTask(3, [&] { order.push_back(3); });
  g.run();
  auto p0 = std::find(order.begin(), order.end(), 0) - order.begin();
  auto p1 = std::find(order.begin(), order.end(), 1) - order.begin();
  auto p2 = std::find(order.begin(), order.end(), 2) - order.begin();
  auto p3 = std::find(order.begin(), order.end(), 3) - order.begin();
  EXPECT_LT(p0, p2);
  EXPECT_LT(p1, p2);
  EXPECT_LT(p2, p3);
}

TEST(StaticPhaseGraph, DynamicOverlayAddsDependency) {
  using Graph = simcore::StaticPhaseGraph<2, kNoEdges>;
  Graph g;
  std::vector<int> order;
  g.setPhaseTask(0, [&] { order.push_back(0); });
  g.setPhaseTask(1, [&] { order.push_back(1); });
  g.addOverlay(0, 1);
  g.run();
  EXPECT_EQ((std::vector<int>{0, 1}), order);
}

TEST(StaticPhaseGraph, AutoRWBarrier) {
  using Graph = simcore::StaticPhaseGraph<2, kNoEdges>;
  Graph g;
  std::vector<int> order;
  std::array<int, 1> buf{0};
  g.setPhaseTask(0, [&] {
    auto w = g.write(0, std::span<int>(buf));
    w[0] = 1;
    order.push_back(0);
  });
  g.setPhaseTask(1, [&] {
    auto r = g.read(1, std::span<const int>(buf));
    (void)r[0];
    order.push_back(1);
  });
  g.run();
  EXPECT_EQ((std::vector<int>{0, 1}), order);
}
