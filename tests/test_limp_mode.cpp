#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>

TEST(LimpMode, DisablesNonEssentialFeatures) {
  SimCore::Settings s;
  s.limpMode = true;
  s.hz = 2000.0; // ensure multiple substeps normally
  s.visualizers = true;
  s.broadphaseCoarse = false;
  s.bintraceEnable = true;
  s.budgetMonitor = true;

  SimCore sim(s);
  EXPECT_FALSE(sim.visualizersEnabled());
  EXPECT_TRUE(sim.broadphaseCoarse());
  EXPECT_EQ(sim.subSteps(), 1);
  EXPECT_FALSE(sim.bintrace().enabled());
}
