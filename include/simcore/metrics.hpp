#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <simcore/worker_pool.hpp>

namespace metrics {

struct CounterSample {
  std::string name;
  std::uint64_t value;
};

struct WorkerPoolSample {
  std::size_t queueMaxDepth = 0;
  std::size_t totalSteals = 0;
  std::vector<std::size_t> stealsPerThread;
};

inline WorkerPoolSample make_sample(WorkerPool::Stats stats) {
  WorkerPoolSample sample;
  sample.queueMaxDepth = stats.maxQueueDepth;
  sample.totalSteals = stats.totalSteals;
  sample.stealsPerThread = std::move(stats.stealsPerThread);
  return sample;
}

inline WorkerPoolSample sample(const WorkerPool &pool) {
  return make_sample(pool.stats());
}

inline std::vector<CounterSample>
worker_pool_counters(const WorkerPoolSample &sample,
                     std::string_view prefix = "worker_pool") {
  std::vector<CounterSample> counters;
  counters.reserve(2 + sample.stealsPerThread.size());
  std::string prefixStr(prefix);
  counters.push_back(
      CounterSample{prefixStr + ".queue_max",
                    static_cast<std::uint64_t>(sample.queueMaxDepth)});
  counters.push_back(
      CounterSample{prefixStr + ".steals_total",
                    static_cast<std::uint64_t>(sample.totalSteals)});
  for (std::size_t i = 0; i < sample.stealsPerThread.size(); ++i) {
    std::ostringstream name;
    name << prefixStr << ".steals[" << i << "]";
    counters.push_back(CounterSample{
        name.str(), static_cast<std::uint64_t>(sample.stealsPerThread[i])});
  }
  return counters;
}

inline std::string worker_pool_json(const WorkerPoolSample &sample) {
  std::ostringstream os;
  os << "{\"queue_max\":" << sample.queueMaxDepth << ",\"steals\":[";
  for (std::size_t i = 0; i < sample.stealsPerThread.size(); ++i) {
    if (i > 0)
      os << ',';
    os << sample.stealsPerThread[i];
  }
  os << "],\"steals_total\":" << sample.totalSteals << "}";
  return os.str();
}

} // namespace metrics

