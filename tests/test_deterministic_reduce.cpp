#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <simcore/SimCore.hpp>
#include <thread>
#include <vector>

struct SumResult {
  double sum{};
  std::vector<std::uint64_t> checks;
};

static SumResult run_sum_with_threads(std::size_t threads) {
  SimCore::Settings s;
  s.threads = threads;
  s.hz = 1000.0;
  s.maxFrames = 1;
  s.mainHelps = true;
  s.chunkSize = 128; // any value; determinism holds

  SimCore sim(s);
  Logger log;
  log.setLevel(Logger::Level::Warn);
  sim.setLogger(&log);

  const std::size_t N = 200000; // enough to be sensitive to order
  std::vector<double> values(N);
  for (std::size_t i = 0; i < N; ++i) {
    // nasty mix to emphasize rounding effects
    values[i] = std::sin(0.001 * double(i)) * 1e-3 +
                std::cos(0.0007 * double(i)) * 1e-6 +
                (i % 7 == 0 ? -1e-9 : 2e-9);
  }

  auto p = sim.addPhase("DetReduce", N);

  SumResult result;
  sim.addDeterministicRangeReduction(
      p,
      // per-chunk local sum
      [&](std::size_t b, std::size_t e, std::int64_t,
          SimCore::Seconds) -> double {
        double sum = 0.0;
        for (std::size_t i = b; i < e; ++i)
          sum += values[i];
        return sum;
      },
      // sink receives pairwise-folded total in a fixed order
      [&](double total, const std::uint64_t *chk, std::size_t n, std::int64_t,
          SimCore::Seconds) {
        result.sum = total;
        result.checks.assign(chk, chk + n);
      });

  sim.run();
  return result;
}

TEST(DeterministicReduction, SumDoubleStableAcrossThreads) {
  SumResult r1 = run_sum_with_threads(1);
  SumResult rN = run_sum_with_threads(
      std::max<std::size_t>(2, std::thread::hardware_concurrency()));

  auto bits = [](double x) {
    std::uint64_t u;
    std::memcpy(&u, &x, sizeof(u));
    return u;
  };

  EXPECT_EQ(bits(r1.sum), bits(rN.sum));
  EXPECT_EQ(r1.checks, rN.checks);
}
