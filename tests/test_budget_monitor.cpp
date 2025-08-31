#include "gtest/gtest.h"
#include "src/SimCore.hpp"
#include <atomic>
#include <thread>

// Helper: busy-wait ~micros microseconds
static void busy_spin(int micros) {
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    while (std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count() < micros) {
        // prevent the compiler from optimizing away
        asm volatile("" ::: "memory");
    }
}

TEST(BudgetMonitor, TriggersCatchupWhenOverBudget)
{
    SimCore::Settings s;
    s.hz           = 500.0;            // 2 ms frame
    s.maxFrames    = 400;              // short run
    s.threads      = 1;                // simpler
    s.mainHelps    = true;
    s.adaptive     = true;             // enable catch-up
    s.maxCatchUp   = 4;
    s.driftLogInterval = 0;            // quieter test output

    SimCore sim(s);

    auto phase = sim.addPhase("Slow");
    sim.setPhaseElementCount(phase, 1);

    // Every frame, burn ~3 ms ( > 2 ms period ), forcing catch-up.
    sim.addSerialSubsystem(phase, [&](std::int64_t, SimCore::Seconds){
        busy_spin(3000);
    });

    sim.run();

    // Expect some catch-up has occurred.
    EXPECT_GT(sim.bursts(), 0);
    EXPECT_GT(sim.extraSteps(), 0);
    EXPECT_GT(sim.recoveredMs(), 0.0);
    // Drift should be negative-ish (real behind sim) or around zero after catch-ups,
    // but at least confirm it's been recorded.
    (void)sim.lastDriftMs();
}
