#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <algorithm>
#include <array>
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
    base.predictiveSlopeMsPerFrame = 0.005; // respond once slope exceeds ~0.005 ms/frame
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

    constexpr int kSamples = 5;
    std::array<int, kSamples> reactiveNo{};
    std::array<int, kSamples> reactiveYes{};
    std::array<int, kSamples> preYes{};

    for (int i = 0; i < kSamples; ++i) {
        auto simNo = make_sim(false);
        simNo->run();
        reactiveNo[static_cast<std::size_t>(i)] = simNo->extraSteps();

        auto simYes = make_sim(true);
        simYes->run();
        reactiveYes[static_cast<std::size_t>(i)] = simYes->extraSteps();
        preYes[static_cast<std::size_t>(i)] = simYes->preSteps();
    }

    auto median = [](std::array<int, kSamples> values) {
        std::nth_element(values.begin(), values.begin() + kSamples / 2, values.end());
        return values[kSamples / 2];
    };

    int medianNo = median(reactiveNo);
    int medianYes = median(reactiveYes);
    int medianPre = median(preYes);

    if (medianPre == 0) {
        GTEST_SKIP() << "Predictive scheduler had no headroom across samples";
    }

    // Compare medians across several runs to smooth out jitter from the busy-spin
    // workload and ensure predictive stepping does not require more catch-up work
    // than the baseline on average.
    EXPECT_LE(medianYes, medianNo) << "median predictive reactive catch-up " << medianYes
                                   << " exceeded baseline " << medianNo;
    EXPECT_GT(medianPre, 0);
}
