#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <simcore/fault_injector.hpp>

TEST(FaultInjection, RandomDelaysStillRun)
{
    SimCore::Settings s;
    s.hz = 200.0; // 5ms
    s.maxFrames = 100;
    s.threads = 1;
    s.adaptive = true;
    s.maxCatchUp = 4;
    SimCore sim(s);
    auto phase = sim.addPhase("delay");
    sim.setPhaseElementCount(phase,1);
    fault::set_delay_probability(0.5);
    fault::set_max_delay_ms(2); // up to 2ms delay
    sim.addSerialSubsystem(phase, [&](int64_t, SimCore::Seconds){ fault::maybe_delay(); });
    sim.run();
    EXPECT_EQ(sim.frame(), s.maxFrames);
}
