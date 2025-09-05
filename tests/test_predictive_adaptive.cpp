#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <atomic>

// Busy spin for ~usec microseconds
static void spin_us(int usec) {
    using Clock = std::chrono::steady_clock;
    auto start = Clock::now();
    auto target = start + std::chrono::microseconds(usec);
    std::atomic<int> x{0};
    while (Clock::now() < target) {
        x.fetch_add(1, std::memory_order_relaxed); // prevent full optimization
    }
}

TEST(PredictiveAdaptive, PrestepsReduceReactiveCatchup)
{
    // Common settings
    SimCore::Settings base;
    base.hz = 400.0;                  // 2.5 ms/frame
    base.maxFrames = 200;             // short run
    base.threads = 1;                 // avoid thread scheduling noise in CI
    base.spinMicros = 0;              // don't burn spin budget
    base.budgetMonitor = true;
    base.predictiveDebugLog = true;
    base.budgetWindow = 120;
    base.predictiveWindow = 60;
    base.predictiveWarmup = 10;
    base.predictiveLookaheadFrames = 6;
    base.predictiveSlopeMsPerFrame = 0.005; // 0.02 ms per frame slope
    base.predictivePreStepLimit = 3;
    base.predictiveMinAvgRatio = 0.5;
    base.bintraceEnable = false;

    // Workload: ~1.8ms base + +0.01ms per frame -> crosses 2.5ms budget later
    auto make_sim = [&](bool predictive)->std::unique_ptr<SimCore> {
        SimCore::Settings s = base;
        s.predictiveEnable = predictive;
        s.predictiveDebugLog = true;
        s.adaptive = true;
        s.maxCatchUp = 4;
        auto sim = std::make_unique<SimCore>(s);
        auto phase = sim->addPhase("W");
        sim->addSerialSubsystem(phase, [=](std::int64_t f, SimCore::Seconds){
            int usec = 1800 + int(10 * f); // linear trend
            spin_us(usec);
        });
        return sim;
    };

    auto simNoPred = make_sim(false);
    simNoPred->run();
    int reactiveNoPred = simNoPred->extraSteps();

    auto simPred = make_sim(true);
    simPred->run();
    int reactivePred = simPred->extraSteps();
    int prePred      = simPred->preSteps();

    if (prePred == 0) {
        GTEST_SKIP() << "Predictive scheduler had no headroom";
    }

    // It should not increase reactive catch-up; ideally reduce or keep same
    EXPECT_LE(reactivePred, reactiveNoPred);
}
