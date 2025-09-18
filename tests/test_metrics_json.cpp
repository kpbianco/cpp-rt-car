#include <gtest/gtest.h>

#include <simcore/SimCore.hpp>
#include <simcore/metrics.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

TEST(MetricsJson, EmitsPhaseStatsAndCounters) {
  SimCore::Settings settings;
  settings.hz = 240.0;
  settings.maxFrames = 6;
  settings.threads = 2;
  settings.predictiveEnable = false;
  settings.adaptive = false;

  SimCore sim(settings);
  metrics::Registry registry;
  sim.setMetrics(&registry);

  auto input = sim.addPhase("Input");
  auto physics = sim.addPhase("Physics");
  constexpr std::size_t kEntityCount = 32;
  sim.setPhaseElementCount(physics, kEntityCount);

  std::vector<double> control(kEntityCount, 0.0);
  std::vector<double> state(kEntityCount, 1.0);

  sim.addSerialSubsystem(input,
                         [&](int64_t frame, SimCore::Seconds dt) {
                           for (std::size_t i = 0; i < control.size(); ++i) {
                             control[i] = std::sin(static_cast<double>(frame) *
                                                   dt.count() +
                                                   static_cast<double>(i) * 0.05);
                           }
                         });

  sim.addParallelRangeTask(
      physics,
      [&](std::size_t begin, std::size_t end, int64_t, SimCore::Seconds) {
        for (std::size_t i = begin; i < end; ++i) {
          state[i] += control[i] * 0.5;
        }
      });

  sim.run();

  auto snapshot = registry.snapshot();
  ASSERT_GE(snapshot.phases.size(), 2u);
  auto inputIt = snapshot.phases.find("Input");
  ASSERT_NE(inputIt, snapshot.phases.end());
  EXPECT_GT(inputIt->second.samples, 0u);

  auto physicsIt = snapshot.phases.find("Physics");
  ASSERT_NE(physicsIt, snapshot.phases.end());
  EXPECT_GT(physicsIt->second.samples, 0u);

  auto counters = snapshot.counters;
  EXPECT_NE(counters.find("missed_frames"), counters.end());
  EXPECT_NE(counters.find("watchdog.trips"), counters.end());
  EXPECT_NE(counters.find("worker.queue_max"), counters.end());
  EXPECT_NE(counters.find("worker.steals_total"), counters.end());
  EXPECT_NE(counters.find("logger.dropped"), counters.end());
  EXPECT_NE(counters.find("thermal.events"), counters.end());

  const std::string json = registry.to_json();
  EXPECT_FALSE(json.empty());
  EXPECT_EQ(json.front(), '{');
  EXPECT_EQ(json.back(), '}');
  EXPECT_EQ(json.find('\n'), std::string::npos);

  auto expectKey = [&](std::string_view key) {
    EXPECT_NE(json.find(std::string(key)), std::string::npos);
  };

  expectKey("\"phases\"");
  expectKey("\"counters\"");
  expectKey("\"p50_ms\"");
  expectKey("\"p95_ms\"");
  expectKey("\"p99_ms\"");
  expectKey("\"missed_frames\"");
  expectKey("\"watchdog.trips\"");
  expectKey("\"worker.queue_max\"");
  expectKey("\"worker.steals_total\"");
  expectKey("\"logger.dropped\"");
  expectKey("\"thermal.events\"");
}
