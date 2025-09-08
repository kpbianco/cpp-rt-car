#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <atomic>

static void busy_spin(int micros) {
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    while (std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count() < micros) {
        std::atomic_signal_fence(std::memory_order_acq_rel);
    }
}

TEST(BudgetMonitor, GracefulDegradeTriggers) {
    SimCore::Settings s;
    s.hz = 3000.0;                 // subSteps > 1
    s.maxFrames = 4;
    s.threads = 1;
    s.budgetMonitor = true;
    s.bintraceEnable = true;
    s.bintraceEventsPerThread = 1u << 10;

    SimCore sim(s);
    auto phase = sim.addPhase("Slow");
    sim.setPhaseElementCount(phase, 1);
    sim.addSerialSubsystem(phase, [&](std::int64_t, SimCore::Seconds){
        busy_spin(450); // force over budget
    });

    sim.run();

    EXPECT_FALSE(sim.visualizersEnabled());
    EXPECT_LT(sim.subSteps(), 3);
    EXPECT_TRUE(sim.broadphaseCoarse());

    auto snap = sim.bintrace().snapshot();
    int rung1 = 0, rung2 = 0, rung3 = 0;
    for (const auto& ev : snap.events) {
        if (ev.code == bintrace::EV_BudgetLadder) {
            if (ev.a == 1) ++rung1;
            if (ev.a == 2) ++rung2;
            if (ev.a == 3) ++rung3;
        }
    }
    EXPECT_EQ(rung1, 1);
    EXPECT_EQ(rung2, 1);
    EXPECT_EQ(rung3, 1);
}

