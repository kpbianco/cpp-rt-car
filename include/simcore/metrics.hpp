#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <map>
#include <mutex>
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

struct Percentiles {
  double p50 = 0.0;
  double p95 = 0.0;
  double p99 = 0.0;
};

class RollingHistogram {
public:
  explicit RollingHistogram(std::size_t capacity = 120)
      : buffer_(capacity, 0.0), capacity_(capacity) {}

  void add(double sample) {
    if (capacity_ == 0)
      return;
    buffer_[next_] = sample;
    next_ = (next_ + 1) % capacity_;
    if (count_ < capacity_)
      ++count_;
  }

  [[nodiscard]] std::size_t sample_count() const { return count_; }

  [[nodiscard]] Percentiles percentiles() const {
    Percentiles p{};
    if (count_ == 0)
      return p;
    auto values = samples();
    std::sort(values.begin(), values.end());
    p.p50 = percentile_from_sorted(values, 0.50);
    p.p95 = percentile_from_sorted(values, 0.95);
    p.p99 = percentile_from_sorted(values, 0.99);
    return p;
  }

private:
  [[nodiscard]] std::vector<double> samples() const {
    std::vector<double> out;
    out.reserve(count_);
    if (capacity_ == 0)
      return out;
    const std::size_t start = (next_ + capacity_ - count_) % capacity_;
    for (std::size_t i = 0; i < count_; ++i) {
      std::size_t idx = (start + i) % capacity_;
      out.push_back(buffer_[idx]);
    }
    return out;
  }

  static double percentile_from_sorted(const std::vector<double> &sorted,
                                       double q) {
    if (sorted.empty())
      return 0.0;
    if (q <= 0.0)
      return sorted.front();
    if (q >= 1.0)
      return sorted.back();
    const double pos = q * static_cast<double>(sorted.size() - 1);
    const std::size_t lower = static_cast<std::size_t>(std::floor(pos));
    const std::size_t upper = static_cast<std::size_t>(std::ceil(pos));
    const double weight = pos - static_cast<double>(lower);
    if (upper >= sorted.size())
      return sorted.back();
    return sorted[lower] + (sorted[upper] - sorted[lower]) * weight;
  }

  std::vector<double> buffer_;
  std::size_t capacity_ = 0;
  std::size_t next_ = 0;
  std::size_t count_ = 0;
};

class Registry {
public:
  struct PhaseSnapshot {
    Percentiles percentiles;
    std::size_t samples = 0;
  };

  struct Snapshot {
    std::map<std::string, PhaseSnapshot> phases;
    std::map<std::string, std::uint64_t> counters;
  };

  explicit Registry(std::size_t histogramCapacity = 120)
      : histogramCapacity_(histogramCapacity == 0 ? 1 : histogramCapacity) {}

  void record_phase(std::string_view name, double milliseconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = phases_.try_emplace(std::string(name), histogramCapacity_).first;
    it->second.histogram.add(milliseconds);
  }

  void set_counter(std::string_view name, std::uint64_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    counters_[std::string(name)] = value;
  }

  void increment_counter(std::string_view name, std::uint64_t delta = 1) {
    std::lock_guard<std::mutex> lock(mutex_);
    counters_[std::string(name)] += delta;
  }

  Snapshot snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot snap;
    for (const auto &kv : phases_) {
      PhaseSnapshot phaseSnap;
      phaseSnap.percentiles = kv.second.histogram.percentiles();
      phaseSnap.samples = kv.second.histogram.sample_count();
      snap.phases.emplace(kv.first, phaseSnap);
    }
    snap.counters = counters_;
    return snap;
  }

  std::string to_json() const {
    const auto snap = snapshot();
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os << std::setprecision(6);
    os << "{\"phases\":{";
    bool first = true;
    for (const auto &kv : snap.phases) {
      if (!first)
        os << ',';
      first = false;
      os << '\"' << escape_json(kv.first) << "\":{\"p50_ms\":"
         << kv.second.percentiles.p50 << ",\"p95_ms\":"
         << kv.second.percentiles.p95 << ",\"p99_ms\":"
         << kv.second.percentiles.p99 << "}";
    }
    os << "},\"counters\":{";
    first = true;
    for (const auto &kv : snap.counters) {
      if (!first)
        os << ',';
      first = false;
      os << '\"' << escape_json(kv.first) << "\":" << kv.second;
    }
    os << "}}";
    return os.str();
  }

private:
  struct PhaseEntry {
    explicit PhaseEntry(std::size_t cap) : histogram(cap) {}
    RollingHistogram histogram;
  };

  static std::string escape_json(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
      switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default: {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20) {
          static constexpr char hex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(hex[(uc >> 4) & 0x0f]);
          out.push_back(hex[uc & 0x0f]);
        } else {
          out.push_back(c);
        }
        break;
      }
      }
    }
    return out;
  }

  std::map<std::string, PhaseEntry> phases_;
  std::map<std::string, std::uint64_t> counters_;
  std::size_t histogramCapacity_;
  mutable std::mutex mutex_;
};

} // namespace metrics

