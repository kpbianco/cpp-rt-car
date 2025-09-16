#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <simcore/logger.hpp>
#include <array>
#include <cstring>
#include <string>
#include <vector>

#include <rt/numerics.hpp>

namespace {

struct DeterminismParam {
    std::size_t threads;
    bool useFma;
};

static std::uint64_t runHash(std::size_t threads, bool useFma) {
    const bool previousFma = rt::use_fma();
    SimCore::Settings s;
    s.hz = 1000.0;
    s.maxFrames = 1500;
    s.threads = threads;
    s.adaptive = false;
    s.driftLogInterval = 0;
    s.spinMicros = 200;
    s.logPhases = false;
    s.logRangeTasks = false;
    s.useFMA = useFma;

    std::uint64_t result = 0;
    {
        Logger logger;
        logger.setLevel(Logger::Level::Error);
        SimCore sim(s);
        sim.setLogger(&logger);

        auto phase = sim.addPhase("Phys");
        const std::size_t N = 5000;
        std::vector<double> pos(N, 0.0), vel(N, 10.0);
        sim.setPhaseElementCount(phase, N);

        sim.addParallelRangeTask(phase, [&](std::size_t b, std::size_t e, int64_t, SimCore::Seconds dt) {
            double d = dt.count();
            for (std::size_t i = b; i < e; ++i) {
                vel[i] += 0.001 * d;
                pos[i] += vel[i] * d;
            }
        });

        sim.addReductionTask(phase, [&](int64_t f, SimCore::Seconds) {
            if (f == s.maxFrames - 1) {
                std::uint64_t h = 1469598103934665603ull;
                for (double v : vel) {
                    std::uint64_t bits;
                    std::memcpy(&bits, &v, sizeof(v));
                    h ^= bits;
                    h *= 1099511628211ull;
                }
                sim.setDeterministicHash(h);
            }
        });

        sim.run();
        result = sim.deterministicHash();
    }

    rt::set_use_fma(previousFma);
    return result;
}

class SimCoreDeterminism : public ::testing::TestWithParam<DeterminismParam> {};

TEST_P(SimCoreDeterminism, HashSameAcrossThreadCounts) {
    auto params = GetParam();
    auto singleThreadHash = runHash(1, params.useFma);
    auto multiThreadHash = runHash(params.threads, params.useFma);
    EXPECT_EQ(singleThreadHash, multiThreadHash);
}

constexpr std::array<std::size_t, 3> kThreadCounts{2, 4, 8};

std::vector<DeterminismParam> buildParams() {
    std::vector<DeterminismParam> params;
    params.reserve(kThreadCounts.size() * 2);
    for (auto threads : kThreadCounts) {
        params.push_back({threads, false});
        if constexpr (rt::detail::kBuildAllowsFma) {
            params.push_back({threads, true});
        }
    }
    return params;
}

const auto kDeterminismParams = buildParams();

std::string determinismParamName(const ::testing::TestParamInfo<DeterminismParam> &info) {
    std::string name = std::to_string(info.param.threads) + "Threads";
    name += info.param.useFma ? "FmaOn" : "FmaOff";
    return name;
}

INSTANTIATE_TEST_SUITE_P(CrossMatrixDeterminism, SimCoreDeterminism,
                         ::testing::ValuesIn(kDeterminismParams),
                         determinismParamName);

} // namespace
