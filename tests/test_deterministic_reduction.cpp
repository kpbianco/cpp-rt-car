#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>

static double run_sum_with_threads(std::size_t threads)
{
    SimCore::Settings s;
    s.threads   = threads;
    s.hz        = 1000.0;
    s.maxFrames = 1;
    s.mainHelps = true;
    s.chunkSize = 128; // any value; determinism holds

    SimCore sim(s);
    Logger log; log.setLevel(Logger::Level::Warn);
    sim.setLogger(&log);

    const std::size_t N = 200000; // enough to be sensitive to order
    std::vector<double> values(N);
    for (std::size_t i = 0; i < N; ++i) {
        // nasty mix to emphasize rounding effects
        values[i] = std::sin(0.001 * double(i)) * 1e-3
                  + std::cos(0.0007 * double(i)) * 1e-6
                  + (i % 7 == 0 ? -1e-9 : 2e-9);
    }

    auto p = sim.addPhase("DetReduce", N);

    double out = 0.0;
    sim.addDeterministicRangeReduction(
        p,
        // per-chunk local sum
        [&](std::size_t b, std::size_t e, std::int64_t, SimCore::Seconds) -> double {
            double s = 0.0;
            for (std::size_t i = b; i < e; ++i) s += values[i];
            return s;
        },
        // sink receives pairwise-folded total in a fixed order
        [&](double total, std::int64_t, SimCore::Seconds){
            out = total;
        }
    );

    sim.run();
    return out;
}

TEST(DeterministicReduction, SumDoubleStableAcrossThreads)
{
    // Compare bit patterns across a few thread counts
    double v1 = run_sum_with_threads(1);
    double v2 = run_sum_with_threads(2);
    double v3 = run_sum_with_threads(3);
    double v7 = run_sum_with_threads(7);

    auto bits = [](double x){
        std::uint64_t u; std::memcpy(&u, &x, sizeof(u)); return u;
    };

    EXPECT_EQ(bits(v1), bits(v2));
    EXPECT_EQ(bits(v1), bits(v3));
    EXPECT_EQ(bits(v1), bits(v7));
}
