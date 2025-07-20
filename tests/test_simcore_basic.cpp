#include <gtest/gtest.h>
#include "simcore.hpp"
#include "logger.hpp"

TEST(SimCoreBasic, RunsExactFrames) {
    SimCore::Settings s;
    s.hz = 500.0;
    s.maxFrames = 600;
    s.threads = 1;
    s.adaptive = false;
    s.driftLogInterval = 0;

    Logger log; log.setLevel(Logger::Level::Error);
    SimCore sim(s);
    sim.setLogger(&log);

    auto phase = sim.addPhase("Empty");
    sim.addSerialSubsystem(phase, [&](int64_t /*f*/, SimCore::Seconds){});

    sim.run();
    EXPECT_EQ(sim.frame(), s.maxFrames);
}
