#include <gtest/gtest.h>
#include <simcore/physics/constraint_solver.hpp>
#include <vector>

using namespace simphys;

TEST(ConstraintSolver, WarmStartCaches) {
  std::vector<Constraint> cons(1);
  SolverSettings s;
  s.iterations = 1;
  s.warmStart = true;
  PGSSolver solver;

  solver.solve(cons, s);
  EXPECT_DOUBLE_EQ(cons[0].lambda, 1.0);

  cons[0].lambda = 0.0; // reset
  s.iterations = 0;     // no solve steps, just warm-start restore
  solver.solve(cons, s);
  EXPECT_DOUBLE_EQ(cons[0].lambda, 1.0);
}

TEST(IslandManager, SleepingHeuristic) {
  IslandManager mgr;
  mgr.sleepThreshold = 1.0;
  IslandState island;
  mgr.update(island, 0.5, false);
  EXPECT_FALSE(island.sleeping);
  mgr.update(island, 0.6, false); // total idle 1.1 > threshold
  EXPECT_TRUE(island.sleeping);
  mgr.update(island, 0.1, true); // active wakes up
  EXPECT_FALSE(island.sleeping);
}
