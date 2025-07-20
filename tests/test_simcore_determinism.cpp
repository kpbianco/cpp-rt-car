#include <gtest/gtest.h>
#include "simcore.hpp"
#include "logger.hpp"
#include <vector>
#include <cstring>

static std::uint64_t runHash(std::size_t threads) {
    SimCore::Settings s;
    s.hz = 1000.0;
    s.maxFrames = 1500;
    s.threads = threads;
    s.adaptive = false;
    s.driftLogInterval = 0;
    s.spinMicros = 200;
    s.logPhases = false;
    s.logRangeTasks = false;

    Logger logger; logger.setLevel(Logger::Level::Error);
    SimCore sim(s);
    sim.setLogger(&logger);

    auto phase = sim.addPhase("Phys");
    const std::size_t N = 5000;
    std::vector<double> pos(N,0.0), vel(N,10.0);
    sim.setPhaseElementCount(phase, N);

    sim.addParallelRangeTask(phase, [&](std::size_t b, std::size_t e, int64_t, SimCore::Seconds dt){
        double d = dt.count();
        for (std::size_t i=b;i<e;++i) {
            vel[i] += 0.001 * d;
            pos[i] += vel[i] * d;
        }
    });

    sim.addReductionTask(phase, [&](int64_t f, SimCore::Seconds){
        if (f == s.maxFrames-1) {
            std::uint64_t h = 1469598103934665603ull;
            for (double v : vel) {
                std::uint64_t bits;
                std::memcpy(&bits, &v, sizeof(v));
                h ^= bits; h *= 1099511628211ull;
            }
            sim.setDeterministicHash(h);
        }
    });

    sim.run();
    return sim.deterministicHash();
}

TEST(SimCoreDeterminism, HashSameAcrossThreadCounts) {
    auto h2 = runHash(2);
    auto h8 = runHash(8);
    EXPECT_EQ(h2, h8);
}
