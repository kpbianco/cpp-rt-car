#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>

TEST(ThermalMonitor, LimpRungReducesSubsteps) {
    SimCore::Settings s;
    s.hz = 3000.0;
    s.maxFrames = 2;
    s.threads = 1;
    s.budgetMonitor = false;
    s.bintraceEnable = true;
    s.thermalMonitor = true;
    s.thermalLimpCelsius = 80.0;
    s.readPackageTemp = [](){ return 100.0; };

    SimCore sim(s);
    auto phase = sim.addPhase("idle");
    sim.addSerialSubsystem(phase, [](std::int64_t, SimCore::Seconds){});
    sim.run();

    EXPECT_EQ(sim.subSteps(), 1);
    auto snap = sim.bintrace().snapshot();
    bool rung4 = false;
    for (const auto& ev : snap.events) {
        if (ev.code == bintrace::EV_BudgetLadder && ev.a == 4)
            rung4 = true;
    }
    EXPECT_TRUE(rung4);
}

