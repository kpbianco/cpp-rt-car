#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <simcore/metrics.hpp>
#include <simcore/worker_pool.hpp>

using namespace std::chrono_literals;

TEST(QueueMetrics, PublishesDepthAndSteals) {
  WorkerPool pool(3, 256);
  std::atomic<int> executed{0};
  constexpr int jobs = 96;

  std::promise<void> releasePromise;
  auto blocker = releasePromise.get_future();

  pool.enqueue([&executed, blocker = std::move(blocker)]() mutable {
    executed.fetch_add(1, std::memory_order_relaxed);
    blocker.wait();
    std::this_thread::sleep_for(2ms);
  });

  for (int i = 1; i < jobs; ++i) {
    pool.enqueue([&] {
      executed.fetch_add(1, std::memory_order_relaxed);
    });
  }

  std::this_thread::sleep_for(5ms);
  releasePromise.set_value();

  pool.drain();
  auto stats = pool.stats();
  auto sample = metrics::make_sample(std::move(stats));
  pool.stop();

  EXPECT_EQ(executed.load(), jobs);
  EXPECT_GT(sample.queueMaxDepth, 0u);
  EXPECT_GT(sample.totalSteals, 0u);
  EXPECT_EQ(sample.stealsPerThread.size(), 3u);

  auto stealSum = std::accumulate(sample.stealsPerThread.begin(),
                                  sample.stealsPerThread.end(), std::size_t{0});
  EXPECT_EQ(stealSum, sample.totalSteals);

  auto json = metrics::worker_pool_json(sample);
  EXPECT_NE(json.find("\"queue_max\""), std::string::npos);
  EXPECT_NE(json.find("\"steals\""), std::string::npos);
  EXPECT_NE(json.find("\"steals_total\""), std::string::npos);
  EXPECT_NE(json.find("\"emergency_spawns\""), std::string::npos);
  EXPECT_NE(json.find(std::to_string(sample.queueMaxDepth)), std::string::npos);

  auto counters = metrics::worker_pool_counters(sample, "pool");
  auto findCounter = [&](std::string_view name) {
    return std::find_if(counters.begin(), counters.end(),
                        [&](const metrics::CounterSample &c) {
                          return c.name == name;
                        });
  };

  auto itQueue = findCounter("pool.queue_max");
  ASSERT_NE(itQueue, counters.end());
  EXPECT_EQ(itQueue->value,
            static_cast<std::uint64_t>(sample.queueMaxDepth));

  auto itSteals = findCounter("pool.steals_total");
  ASSERT_NE(itSteals, counters.end());
  EXPECT_EQ(itSteals->value,
            static_cast<std::uint64_t>(sample.totalSteals));

  auto itEmergency = findCounter("pool.emergency_spawns");
  ASSERT_NE(itEmergency, counters.end());
  EXPECT_EQ(itEmergency->value, sample.emergencySpawns);
}

