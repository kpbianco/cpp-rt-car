#include <gtest/gtest.h>
#include "simcore.hpp"
#include "logger.hpp"

TEST(AdaptiveParamTest, DriftBounded) {
    SimCore::Settings s;
    s.hz = 1000.0;
    s.maxFrames = 1500;
    s.adaptive = true;
    s.threads = 2;
    s.driftLogInterval = 0;
    s.spinMicros = 200;

    Logger log; log.setLevel(Logger::Level::Error);
    SimCore sim(s);
    sim.setLogger(&log);
    auto phase = sim.addPhase("Empty");

    sim.run();
    // Some generous bound; depends on platform jitter.
    EXPECT_LT(std::fabs(sim.lastDriftMs()), 5.0);
}
