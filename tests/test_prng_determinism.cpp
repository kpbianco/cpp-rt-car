#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <simcore/logger.hpp>
#include <vector>
#include <cstring>

static std::uint64_t run_hash(std::size_t threads, bool useFMA) {
    SimCore::Settings s;
    s.hz = 1000.0;
    s.maxFrames = 1;
    s.threads = threads;
    s.adaptive = false;
    s.driftLogInterval = 0;
    s.spinMicros = 0;
    s.logPhases = false;
    s.logRangeTasks = false;
    s.rngSeed = 123456789u;
    s.useFMA = useFMA;

    Logger logger; logger.setLevel(Logger::Level::Error);
    SimCore sim(s);
    sim.setLogger(&logger);

    auto phase = sim.addPhase("Rng");
    const std::size_t N = 1024;
    std::vector<std::uint32_t> vals(N, 0);
    sim.setPhaseElementCount(phase, N);

    sim.addParallelRangeTask(phase, [&](std::size_t b, std::size_t e, int64_t f, SimCore::Seconds){
        for (std::size_t i = b; i < e; ++i)
            vals[i] =
                sim.prng(static_cast<std::uint64_t>(f), static_cast<std::uint64_t>(i));
    });

    sim.addReductionTask(phase, [&](int64_t, SimCore::Seconds){
        std::uint64_t h = 1469598103934665603ull;
        for (auto v: vals) { h ^= v; h *= 1099511628211ull; }
        sim.setDeterministicHash(h);
    });

    sim.run();
    return sim.deterministicHash();
}

TEST(PRNGDeterminism, SameAcrossThreads) {
    auto h1 = run_hash(1, false);
    auto h8 = run_hash(8, false);
    EXPECT_EQ(h1, h8);
}

TEST(PRNGDeterminism, FmaOnOff) {
    auto h_off = run_hash(4, false);
    auto h_on = run_hash(4, true);
    EXPECT_EQ(h_off, h_on);
}
