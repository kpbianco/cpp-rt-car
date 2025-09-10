// SimCore.hpp — baseline w/ predictive priming, headroom math, frame
// PROF_SCOPE, and small-array parallel cut-off (minParallelElems /
// minParallelChunks).
#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <memory_resource>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "bintrace.hpp"
#include "frame_arena.hpp"
#include "logger.hpp"
#include "profiler.hpp"
#include "highres_clock.hpp"
#include "worker_pool.hpp"
#include <rt/fiber_pool.hpp>
#include <rt/numerics.hpp>
#include <rt/prng.hpp>
#include <rt/snapshot.hpp>

#ifndef _WIN32
#include <sched.h>
#endif
#ifdef __linux__
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef SIM_USE_NUMA
#include <numa.h>
#endif
#endif

namespace simcore_det {
template <class Vec> inline double pairwise_sum(Vec &vals) {
  std::size_t len = vals.size();
  if (len == 0)
    return 0.0;
  while (len > 1) {
    const std::size_t half = len / 2;
    for (std::size_t i = 0; i < half; ++i)
      vals[i] += vals[i + half];
    if (len & 1)
      vals[half] = vals[len - 1];
    len = half + (len & 1);
  }
  return vals[0];
}
} // namespace simcore_det

// PMR adaptor for FrameArena
struct ArenaResource : std::pmr::memory_resource {
  FrameArena *arena;
  explicit ArenaResource(FrameArena &a) : arena(&a) {}

private:
  void *do_allocate(std::size_t bytes, std::size_t align) override {
    return arena->allocate(bytes, align);
  }
  void do_deallocate(void *, std::size_t, std::size_t) override {}
  bool
  do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
    return this == &other;
  }
};

// Erased call refs
struct SerialFnRef {
  void (*fn)(void *, std::int64_t, std::chrono::duration<double>);
  void *ctx{};
};

enum class TaskHint { Throughput, Latency };

struct RangeFnRef {
  void (*fn)(void *, std::size_t, std::size_t, std::int64_t,
             std::chrono::duration<double>);
  void *ctx{};
  TaskHint hint{TaskHint::Throughput};
};

struct RedFnRef {
  void (*fn)(void *, std::int64_t, std::chrono::duration<double>);
  void *ctx{};
};
struct DetRangeReductionRef {
  double (*leaf)(void *, std::size_t, std::size_t, std::int64_t,
                 std::chrono::duration<double>);
  void *leafCtx{};
  void (*sink)(void *, double, const std::uint64_t *, std::size_t, std::int64_t,
               std::chrono::duration<double>);
  void *sinkCtx{};
};

class SimCore {
public:
  using Seconds = std::chrono::duration<double>;
  using Clock = HighResClock;
  using TaskHint = ::TaskHint;

  int bursts() const { return bursts_; }
  int extraSteps() const { return extraSteps_; }
  int preSteps() const { return preSteps_; }
  double recoveredMs() const { return recoveredMs_; }
  double precoveredMs() const { return precoveredMs_; }
  FrameArena &frameArena() { return frameArenas_->tls(); }
  rt::FiberPool &fiberPool() { return *fiberPool_; }
  bintrace::Trace &bintrace() { return bintrace_; }
  bool visualizersEnabled() const { return settings_.visualizers; }
  bool broadphaseCoarse() const { return settings_.broadphaseCoarse; }
  int subSteps() const { return subSteps_; }

  // Counter-based deterministic PRNG. Counter is derived from frame and
  // element index so results are independent of execution order.
  std::uint32_t prng(std::uint64_t frame, std::uint64_t index,
                     std::uint64_t counter = 0) const {
    std::uint64_t ctr = (frame << 32) ^ (index & 0xffffffffull);
    ctr += counter;
    return rt::prng::uniform_u32(rngSeed_, ctr);
  }

  struct Settings {
    // Affinity / NUMA
    bool pinThreads = false;
    bool compactNUMA = false;

    // Cadence
    double hz = 500.0;
    std::int64_t maxFrames = 2500;

    // Reactive catch-up
    bool adaptive = false;
    int maxCatchUp = 4;

    // Workers / chunking
    std::size_t threads = std::thread::hardware_concurrency();
    bool mainHelps = true;
    std::size_t chunkSize = 256;

    // Logging / profiling
    int driftLogInterval = 250;
    int spinMicros = 200;
    bool logPhases = false;
    bool logRangeTasks = false;
    bool logChunks = false;
    std::size_t arenaPerThreadBytes = 1u << 20;

    // Adaptive burst accounting
    double adaptiveThresholdFrames = 1.0;

    // Auto chunk tuning
    bool autoTuneChunks = true;
    int targetChunksPerThread = 2;
    std::size_t minChunk = 64;
    std::size_t maxChunk = 8192;
    double chunkPercentile = 0.9;        // percentile target [0,1]
    int chunkSampleFrames = 20;          // frames to sample before pinning
    double chunkDriftThreshold = 1000.0; // CUSUM threshold for drift
    std::string chunkCacheFile;          // optional cache file base path

    // Small-array parallel cut-offs (avoid over-partitioning)
    // If minParallelElems==0 -> auto = max(2*minChunk, minChunk)
    std::size_t minParallelElems = 0;
    std::size_t minParallelChunks = 1;

    // Deterministic reduction leaf size (elements)
    std::size_t detReduceLeaf = 2048;

    // Numeric control
    bool useFMA = false;       // gate for fused multiply-add
    std::uint64_t rngSeed = 0; // base seed for counter PRNG

    // Time Budget Monitor
    bool budgetMonitor = true;
    int budgetWindow = 120;
    double budgetWarnRatio = 0.85;
    double budgetCritRatio = 1.00;
    bool autoAdaptHz = false;
    double adaptDownFactor = 0.95;
    double minHz = 60.0;
    int adaptCooldownFrames = 600;

    // Thermal monitoring
    bool thermalMonitor = false;
    double thermalLimpCelsius = 95.0;
    std::function<double()> readPackageTemp;

    // Graceful degradation
    bool limpMode = false; // global switch to minimize workload
    bool visualizers = true;
    bool broadphaseCoarse = false;

    // Binary event trace
    bool bintraceEnable = false;
    std::size_t bintraceEventsPerThread = 1u << 18;

    // Predictive / Feed-Forward
    bool predictiveEnable = true;
    int predictiveWindow = 60;                // samples for slope
    int predictiveWarmup = 20;                // min samples to act
    int predictiveLookaheadFrames = 8;        // forecast horizon
    double predictiveSlopeMsPerFrame = 0.005; // slope trigger
    int predictivePreStepLimit = 2;           // max per outer frame
    double predictiveMinAvgRatio = 0.70;      // require some load
    bool predictiveDebugLog = false;          // optional logging
  };

  using Subsystem = std::function<void(std::int64_t, Seconds)>;
  using RangeTask =
      std::function<void(std::size_t, std::size_t, std::int64_t, Seconds)>;
  using ReductionTask = std::function<void(std::int64_t, Seconds)>;

  struct DetRangeReduction {
    std::function<double(std::size_t, std::size_t, std::int64_t, Seconds)> fn;
    std::function<void(double, const std::uint64_t *, std::size_t, std::int64_t,
                       Seconds)>
        sink;
  };

  struct Phase {
    std::string name;
    std::vector<SerialFnRef> serials;
    std::vector<RangeFnRef> ranges;
    std::vector<RedFnRef> reductions;
    std::vector<DetRangeReductionRef> detRanges;
    std::vector<std::shared_ptr<void>> keep;
    std::size_t elementCount = 0;
    bool enabled = true;
    std::size_t chosenChunk = 0;
    std::size_t totalChunks = 0;
    std::size_t chunkSkew = 0;
    std::vector<std::size_t> chunkSamples;
    double chunkCusum = 0.0;
    bool chunkPinned = false;
    int numaNode = -1;
  };

  struct BudgetSample {
    double computeMs = 0.0;
    double periodMs = 0.0;
    double ratio = 0.0;
    double avgComputeMs = 0.0;
    double avgRatio = 0.0;
    std::int64_t frame = 0;
  };

  SimCore() : SimCore(Settings{}) {}
  explicit SimCore(const Settings &s) {
#ifndef _WIN32
    char host[256];
    if (gethostname(host, sizeof(host)) == 0)
      machineId_ = host;
    else
      machineId_ = "unknown";
#else
    machineId_ = "windows";
#endif
    HighResClock::init();
    applySettings(s);
    initThreads();
  }
  ~SimCore() {
    saveChunkCache();
    stopThreads();
  }

  void setLogger(Logger *l) { logger_ = l; }
  void setProfiler(Profiler *p) { profiler_ = p; }
  void setWorkerPool(WorkerPool *pool) { pool_ = pool; }
  void setBudgetCallback(std::function<void(const BudgetSample &)> cb) {
    budgetCb_ = std::move(cb);
  }

  void applySettings(const Settings &s) {
    settings_ = s;
    rngSeed_ = settings_.rngSeed;
    rt::set_use_fma(settings_.useFMA);
    if (settings_.hz <= 0.0)
      settings_.hz = 1.0;
    if (settings_.threads == 0)
      settings_.threads = 1;
    if (settings_.maxCatchUp < 0)
      settings_.maxCatchUp = 0;

    if (settings_.predictiveWindow <= 0)
      settings_.predictiveWindow = 1;
    if (settings_.predictiveWindow > settings_.budgetWindow)
      settings_.predictiveWindow = settings_.budgetWindow;

    recalcTiming();

    if (settings_.limpMode) {
      settings_.budgetMonitor = false;
      settings_.bintraceEnable = false;
      applyDegradeRung(4);
    }

    costWindow_.assign(
        static_cast<std::size_t>(std::max(0, settings_.budgetWindow)), 0.0);
    costHead_ = 0;
    costCount_ = 0;
    costSumMs_ = 0.0;
    predictivePrimed_ = false;

    if (!threads_.empty() && settings_.threads != workerCount_) {
      stopThreads();
      initThreads();
    }
    LOG_INFO(logger_,
             "Config hz={} maxFrames={} threads={} predictive={} lookahead={} "
             "slope={}ms/f preLimit={} minParElems={} minParChunks={}",
             settings_.hz, settings_.maxFrames, settings_.threads,
             settings_.predictiveEnable, settings_.predictiveLookaheadFrames,
             settings_.predictiveSlopeMsPerFrame,
             settings_.predictivePreStepLimit, settings_.minParallelElems,
             settings_.minParallelChunks);
    loadChunkCache();
    graphDirty_ = true;
  }

#ifdef __linux__
  inline bool set_affinity(std::size_t cpu) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    return ::sched_setaffinity(0, sizeof(mask), &mask) == 0;
  }
#endif

  // Phase API
  std::size_t addPhase(const std::string &name, std::size_t elemCount = 0,
                       int numaNode = -1) {
    phases_.emplace_back(Phase{name,
                               {},
                               {},
                               {},
                               {},
                               {},
                               elemCount,
                               true,
                               0,
                               0,
                               0,
                               {},
                               0.0,
                               false,
                               numaNode});
    succ_.push_back({});
    indegree_.push_back(0);
    graphDirty_ = true;
    LOG_DEBUG(logger_, "AddPhase '{}' elemCount={}", name, elemCount);
    return phases_.size() - 1;
  }
  void setPhaseElementCount(std::size_t phaseIndex, std::size_t count) {
    phases_[phaseIndex].elementCount = count;
    graphDirty_ = true;
    LOG_DEBUG(logger_, "Phase '{}' set elementCount={}",
              phases_[phaseIndex].name, count);
  }
  void setPhaseNUMANode(std::size_t phaseIndex, int node) {
    phases_[phaseIndex].numaNode = node;
  }
  void addSerialSubsystem(std::size_t phaseIndex, Subsystem fn) {
    using F = std::function<void(std::int64_t, Seconds)>;
    auto sp = std::make_shared<F>(std::move(fn));
    phases_[phaseIndex].keep.push_back(sp);
    SerialFnRef ref{[](void *c, std::int64_t fr, Seconds dt) {
                      (*static_cast<F *>(c))(fr, dt);
                    },
                    sp.get()};
    phases_[phaseIndex].serials.push_back(ref);
  }
  void addParallelRangeTask(std::size_t phaseIndex, RangeTask fn,
                            TaskHint hint = TaskHint::Throughput) {
    using F =
        std::function<void(std::size_t, std::size_t, std::int64_t, Seconds)>;
    auto sp = std::make_shared<F>(std::move(fn));
    phases_[phaseIndex].keep.push_back(sp);
    RangeFnRef ref{[](void *c, std::size_t b, std::size_t e, std::int64_t fr,
                      Seconds dt) { (*static_cast<F *>(c))(b, e, fr, dt); },
                   sp.get(), hint};
    phases_[phaseIndex].ranges.push_back(ref);
  }
  void addReductionTask(std::size_t phaseIndex, ReductionTask fn) {
    using F = std::function<void(std::int64_t, Seconds)>;
    auto sp = std::make_shared<F>(std::move(fn));
    phases_[phaseIndex].keep.push_back(sp);
    RedFnRef ref{[](void *c, std::int64_t fr, Seconds dt) {
                   (*static_cast<F *>(c))(fr, dt);
                 },
                 sp.get()};
    phases_[phaseIndex].reductions.push_back(ref);
  }
  void addDeterministicRangeReduction(
      std::size_t phaseIndex,
      std::function<double(std::size_t, std::size_t, std::int64_t, Seconds)>
          leaf,
      std::function<void(double, const std::uint64_t *, std::size_t,
                         std::int64_t, Seconds)>
          sink) {
    using LeafF =
        std::function<double(std::size_t, std::size_t, std::int64_t, Seconds)>;
    using SinkF = std::function<void(double, const std::uint64_t *, std::size_t,
                                     std::int64_t, Seconds)>;
    auto leafSp = std::make_shared<LeafF>(std::move(leaf));
    auto sinkSp = std::make_shared<SinkF>(std::move(sink));
    phases_[phaseIndex].keep.push_back(leafSp);
    phases_[phaseIndex].keep.push_back(sinkSp);
    DetRangeReductionRef ref;
    ref.leafCtx = leafSp.get();
    ref.sinkCtx = sinkSp.get();
    ref.leaf = [](void *c, std::size_t b, std::size_t e, std::int64_t fr,
                  Seconds dt) -> double {
      return (*static_cast<LeafF *>(c))(b, e, fr, dt);
    };
    ref.sink = [](void *c, double total, const std::uint64_t *chk,
                  std::size_t n, std::int64_t fr, Seconds dt) {
      (*static_cast<SinkF *>(c))(total, chk, n, fr, dt);
    };
    phases_[phaseIndex].detRanges.push_back(ref);
  }

  bool addDependency(std::size_t before, std::size_t after) {
    if (before >= phases_.size() || after >= phases_.size() || before == after)
      return false;
    auto &s = succ_[before];
    if (std::find(s.begin(), s.end(), after) != s.end())
      return false;
    s.push_back(after);
    indegree_[after] += 1;
    graphDirty_ = true;
    return true;
  }

  void setDeterministicHash(std::uint64_t h) { deterministicHash_ = h; }
  std::uint64_t deterministicHash() const { return deterministicHash_; }
  std::size_t phaseChunk(std::size_t phaseIndex) const {
    return phases_[phaseIndex].chosenChunk;
  }
  bool phaseChunkPinned(std::size_t phaseIndex) const {
    return phases_[phaseIndex].chunkPinned;
  }

  void requestExit() { terminate_ = true; }
  std::int64_t frame() const { return frame_; }
  double dtSeconds() const { return dt_.count(); }
  double lastDriftMs() const { return lastDriftMs_; }

  std::vector<std::uint8_t> saveFrame() const {
    rt::SnapshotWriter w;
    w.write<std::uint64_t>(static_cast<std::uint64_t>(frame_));
    w.write<std::uint64_t>(rngSeed_);
    w.write<std::uint64_t>(static_cast<std::uint64_t>(subSteps_));
    w.write<std::uint64_t>(static_cast<std::uint64_t>(preSteps_));
    std::uint64_t phCount = phases_.size();
    w.write<std::uint64_t>(phCount);
    for (const auto &ph : phases_) {
      w.writeVector(ph.chunkSamples);
      w.write<std::uint64_t>(static_cast<std::uint64_t>(ph.elementCount));
      std::uint8_t en = ph.enabled ? 1 : 0;
      w.write(en);
      w.write<std::uint64_t>(static_cast<std::uint64_t>(ph.chosenChunk));
      w.write<std::uint64_t>(static_cast<std::uint64_t>(ph.totalChunks));
      w.write<std::uint64_t>(static_cast<std::uint64_t>(ph.chunkSkew));
      w.write(ph.chunkCusum);
      std::uint8_t pinned = ph.chunkPinned ? 1 : 0;
      w.write(pinned);
    }
    w.writeVector(costWindow_);
    w.write<std::uint64_t>(static_cast<std::uint64_t>(costHead_));
    w.write<std::uint64_t>(static_cast<std::uint64_t>(costCount_));
    w.write(costSumMs_);
    std::uint8_t prim = predictivePrimed_ ? 1 : 0;
    w.write(prim);
    w.write<std::uint64_t>(static_cast<std::uint64_t>(degradeRung_));
    w.write<std::uint64_t>(static_cast<std::uint64_t>(bursts_));
    w.write<std::uint64_t>(static_cast<std::uint64_t>(extraSteps_));
    w.write(recoveredMs_);
    w.write(precoveredMs_);
    return w.data;
  }

  void loadFrame(const std::vector<std::uint8_t> &data) {
    rt::SnapshotReader r(data);
    std::uint64_t tmp = 0;
    r.read(tmp);
    frame_ = static_cast<std::int64_t>(tmp);
    r.read(rngSeed_);
    r.read(tmp);
    subSteps_ = static_cast<int>(tmp);
    r.read(tmp);
    preSteps_ = static_cast<int>(tmp);
    std::uint64_t phSaved = 0;
    r.read(phSaved);
    for (std::uint64_t i = 0; i < phSaved; ++i) {
      std::vector<std::size_t> samples;
      r.readVector(samples);
      r.read(tmp);
      std::size_t elem = static_cast<std::size_t>(tmp);
      std::uint8_t en = 0;
      r.read(en);
      r.read(tmp);
      std::size_t chosen = static_cast<std::size_t>(tmp);
      r.read(tmp);
      std::size_t totalChunks = static_cast<std::size_t>(tmp);
      r.read(tmp);
      std::size_t chunkSkew = static_cast<std::size_t>(tmp);
      double cusum = 0.0;
      r.read(cusum);
      std::uint8_t pinned = 0;
      r.read(pinned);
      if (i < phases_.size()) {
        auto &ph = phases_[i];
        ph.chunkSamples = std::move(samples);
        ph.elementCount = elem;
        ph.enabled = (en != 0);
        ph.chosenChunk = chosen;
        ph.totalChunks = totalChunks;
        ph.chunkSkew = chunkSkew;
        ph.chunkCusum = cusum;
        ph.chunkPinned = (pinned != 0);
      }
    }
    r.readVector(costWindow_);
    r.read(tmp);
    costHead_ = static_cast<std::size_t>(tmp);
    r.read(tmp);
    costCount_ = static_cast<std::size_t>(tmp);
    r.read(costSumMs_);
    std::uint8_t prim = 0;
    r.read(prim);
    predictivePrimed_ = prim != 0;
    r.read(tmp);
    degradeRung_ = static_cast<int>(tmp);
    r.read(tmp);
    bursts_ = static_cast<int>(tmp);
    r.read(tmp);
    extraSteps_ = static_cast<int>(tmp);
    r.read(recoveredMs_);
    r.read(precoveredMs_);
  }

  void run() {
    startReal_ = Clock::now();
    accumulator_ = Seconds{0};
    nextTarget_ = startReal_;
    while (step()) {
    }
  }

private:
  struct ActiveRange {
    RangeFnRef *fn = nullptr;
    std::size_t totalChunks = 0;
    std::size_t elementCount = 0;
    std::size_t chunkSize = 0;
    std::int64_t frame = 0;
    Seconds dt{};
  };

  void initThreads() {
    stopThreads();
    workerCount_ = settings_.threads;
    std::vector<int> threadNodes(workerCount_ + 1, -1);

    bintrace_.init(workerCount_ + 1, settings_.bintraceEventsPerThread,
                   settings_.bintraceEnable);

    shutdown_.store(false, std::memory_order_relaxed);

#ifdef __linux__
    std::vector<int> cpuList;
    if (settings_.pinThreads) {
      const long nCpusL = ::sysconf(_SC_NPROCESSORS_ONLN);
      const int nCpus =
          (nCpusL > static_cast<long>(std::numeric_limits<int>::max()))
              ? std::numeric_limits<int>::max()
              : static_cast<int>(nCpusL);
#ifdef SIM_USE_NUMA
      if (settings_.compactNUMA && numa_available() != -1) {
        const int maxNode = numa_max_node();
        for (int node = 0; node <= maxNode; ++node) {
          struct bitmask *bm = numa_allocate_cpumask();
          numa_node_to_cpus(node, bm);
          for (int c = 0; c < nCpus; ++c)
            if (numa_bitmask_isbitset(bm, c))
              cpuList.push_back(c);
          numa_free_cpumask(bm);
        }
      } else
#endif
      {
        for (int c = 0; c < nCpus; ++c)
          cpuList.push_back(c);
      }
    }
#ifdef SIM_USE_NUMA
    if (numa_available() != -1 && !cpuList.empty()) {
      for (std::size_t i = 0; i < workerCount_; ++i) {
        int cpu = cpuList[i % cpuList.size()];
        threadNodes[i] = numa_node_of_cpu(cpu);
      }
      int mainCpu = ::sched_getcpu();
      if (mainCpu >= 0)
        threadNodes[workerCount_] = numa_node_of_cpu(mainCpu);
    }
#endif
#endif

    frameArenas_ = std::make_unique<FrameArenaPool>(
        workerCount_ + 1, settings_.arenaPerThreadBytes, 64, threadNodes);

    // Worker threads are provided by WorkerPool.  We only bind the main
    // thread here; worker threads will bind themselves when executing
    // jobs.
    frameArenas_->bindCurrentThread(workerCount_); // main
    bintrace_.bindThread(workerCount_);
    rt::init_fp_env();
  }

  void stopThreads() {
    threads_.clear();
    bintrace_.shutdown();
  }

  void workerLoop() {
    std::uint64_t localToken = dispatchToken_.load(std::memory_order_acquire);
    for (;;) {
      while (localToken == dispatchToken_.load(std::memory_order_acquire) &&
             !shutdown_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      if (shutdown_.load(std::memory_order_acquire))
        break;
      localToken = dispatchToken_.load(std::memory_order_acquire);
      processActiveRange();
    }
  }

  void processActiveRange() {
    for (;;) {
      std::size_t idx = nextChunk_.fetch_add(1, std::memory_order_relaxed);
      if (idx >= active_.totalChunks)
        break;
      std::size_t b = idx * active_.chunkSize;
      std::size_t e = std::min(b + active_.chunkSize, active_.elementCount);

      if (settings_.logChunks) {
        // lightweight binary log already present; text under flag only
        LOG_TRACE(logger_, "ChunkStart idx={} b={} e={}", idx, b, e);
      }

      bintrace_.log(bintrace::EV_ChunkStart,
                    static_cast<std::uint32_t>(active_.chunkSize),
                    (static_cast<std::uint64_t>(active_.frame) << 32) |
                        static_cast<std::uint64_t>(idx));

      active_.fn->fn(active_.fn->ctx, b, e, active_.frame, active_.dt);

      bintrace_.log(bintrace::EV_ChunkDone, static_cast<std::uint32_t>(e - b),
                    (static_cast<std::uint64_t>(active_.frame) << 32) |
                        static_cast<std::uint64_t>(idx));

      if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1)
        break;
    }
  }

  std::string cachePath() const {
    if (settings_.chunkCacheFile.empty())
      return "";
    return settings_.chunkCacheFile + "." + machineId_;
  }

  void loadChunkCache() {
    chunkCache_.clear();
    std::string path = cachePath();
    if (path.empty())
      return;
    std::ifstream in(path);
    if (!in)
      return;
    std::string phase;
    std::size_t chunk;
    while (in >> phase >> chunk)
      chunkCache_[phase] = chunk;
  }

  void saveChunkCache() {
    std::string path = cachePath();
    if (path.empty())
      return;
    std::ofstream out(path);
    if (!out)
      return;
    for (auto &p : chunkCache_)
      out << p.first << " " << p.second << "\n";
  }

  std::size_t pickChunk(std::size_t phaseIdx, std::size_t elems,
                        TaskHint hint) {
    auto &ph = phases_[phaseIdx];
    if (!settings_.autoTuneChunks) {
      std::size_t chunk = std::max<std::size_t>(
          1, settings_.chunkSize ? settings_.chunkSize : 256);
      if (hint == TaskHint::Latency)
        chunk = std::max<std::size_t>(1, chunk / 2);
      return chunk;
    }

    const std::size_t threads = std::max<std::size_t>(1, workerCount_);
    std::size_t targetChunks = std::max<std::size_t>(
        1, threads * static_cast<std::size_t>(
                         std::max(1, settings_.targetChunksPerThread)));
    if (hint == TaskHint::Latency)
      targetChunks *= 2;

    std::size_t calc = (elems + targetChunks - 1) / targetChunks; // ceil
    calc = std::clamp(calc, settings_.minChunk, settings_.maxChunk);
    if (calc > elems)
      calc = elems;
    if (calc == 0)
      calc = 1;

    if (ph.chunkPinned) {
      double diff =
          static_cast<double>(calc) - static_cast<double>(ph.chosenChunk);
      ph.chunkCusum += diff;
      if (std::fabs(ph.chunkCusum) > settings_.chunkDriftThreshold) {
        ph.chunkPinned = false;
        ph.chunkSamples.clear();
        ph.chunkCusum = 0.0;
        ph.chosenChunk = calc;
        ph.chunkSamples.push_back(calc);
      } else {
        return ph.chosenChunk;
      }
    } else if (!ph.chunkPinned) {
      auto it = chunkCache_.find(ph.name);
      if (it != chunkCache_.end()) {
        ph.chosenChunk = it->second;
        ph.chunkPinned = true;
        return ph.chosenChunk;
      }
    }

    ph.chunkSamples.push_back(calc);
    if (ph.chunkSamples.size() >=
        static_cast<std::size_t>(std::max(1, settings_.chunkSampleFrames))) {
      auto temp = ph.chunkSamples;
      std::size_t idx = static_cast<std::size_t>(
          std::clamp(settings_.chunkPercentile, 0.0, 1.0) *
          static_cast<double>(temp.size() - 1));
      std::nth_element(temp.begin(),
                       temp.begin() + static_cast<std::ptrdiff_t>(idx),
                       temp.end());
      ph.chosenChunk = temp[idx];
      ph.chunkPinned = true;
      ph.chunkSamples.clear();
      chunkCache_[ph.name] = ph.chosenChunk;
      saveChunkCache();
      return ph.chosenChunk;
    }

    ph.chosenChunk = calc;
    return calc;
  }

  // --- Predictive helpers -------------------------------------------------

  double computeSlopeMsPerFrame() const {
    const int N =
        std::min<int>(static_cast<int>(costCount_), settings_.predictiveWindow);
    if (N <= 1)
      return 0.0;

    double sumY = 0.0, sumIY = 0.0;
    long long sumI = 0, sumI2 = 0;

    int start = static_cast<int>(costHead_) - N;
    if (start < 0)
      start += static_cast<int>(costWindow_.size());

    for (int k = 0; k < N; ++k) {
      int idx = start + k;
      if (idx >= (int)costWindow_.size()) {
        idx -= (int)costWindow_.size();
      }
      double y = costWindow_[static_cast<std::size_t>(idx)];
      int i = k;
      sumY += y;
      sumI += i;
      sumI2 += 1LL * i * i;
      sumIY += (double)i * y;
    }

    const double denom =
        (double)N * (double)sumI2 - (double)sumI * (double)sumI;
    if (denom <= 0.0)
      return 0.0;
    return ((double)N * sumIY - (double)sumI * sumY) / denom; // ms/frame
  }

  int trendUpCount(int M) const {
    const int available =
        std::min<int>(static_cast<int>(costCount_), settings_.predictiveWindow);
    if (available <= 1)
      return 0;
    M = std::clamp(M, 1, available - 1);

    int up = 0;
    int idxNew = static_cast<int>(costHead_) - 1;
    if (idxNew < 0)
      idxNew += static_cast<int>(costWindow_.size());
    double prev = costWindow_[static_cast<std::size_t>(idxNew)];

    for (int k = 1; k <= M; ++k) {
      int idx = idxNew - k;
      if (idx < 0)
        idx += static_cast<int>(costWindow_.size());
      double val = costWindow_[static_cast<std::size_t>(idx)];
      if (prev > val)
        ++up; // increasing towards present
      prev = val;
    }
    return up;
  }

  int decidePreSteps(double slopeMsPerFrame) const {
    // warmup
    if (costCount_ <
        static_cast<std::size_t>(std::max(settings_.predictiveWarmup, 2)))
      return 0;

    // If trend isn't strong, do nothing
    if (slopeMsPerFrame < settings_.predictiveSlopeMsPerFrame)
      return 0;

    // Only act if we're below critical budget and the average load is high
    // enough to warrant intervention.
    const double periodMs = 1000.0 / settings_.hz;
    const double avgComputeMs =
        (costCount_ ? (costSumMs_ / double(costCount_)) : 0.0);
    const double avgRatio = (periodMs > 0.0) ? (avgComputeMs / periodMs) : 0.0;
    if (avgRatio >= settings_.budgetCritRatio)
      return 0;
    if (avgRatio < settings_.predictiveMinAvgRatio)
      return 0;

    // Scale the number of pre-steps with the observed slope, capped by the
    // configured limit.
    int steps = static_cast<int>(
        std::ceil(slopeMsPerFrame / settings_.predictiveSlopeMsPerFrame));
    if (steps <= 0)
      return 0;
    return std::clamp(steps, 0, settings_.predictivePreStepLimit);
  }

  // --- main step ----------------------------------------------------------
  bool step() {
    if (terminate_)
      return false;
    if (settings_.maxFrames >= 0 && frame_ >= settings_.maxFrames)
      return false;

    // Frame-level profiler scope
    PROF_SCOPE(profiler_, "Frame");

    uint64_t computeStart = Clock::now();
    executeFrame();
    uint64_t computeEnd = Clock::now();

    const double computeMs = Clock::to_ms(computeEnd - computeStart);
    updateBudget(computeMs);

    // feed-forward: build headroom (do BEFORE sleeping)
    if (settings_.predictiveEnable) {
      // headroom to the *next* frame target (not just-finished frame)
      uint64_t nowBeforePre = Clock::now();
      const uint64_t nextFrameTarget = nextTarget_ + dtNs_;
      const int64_t aheadDur = static_cast<int64_t>(nextFrameTarget) -
                               static_cast<int64_t>(nowBeforePre);
      const double aheadFr =
          (static_cast<double>(aheadDur) / 1e9) / dt_.count();

      // decide desired number (based on slope, warmup, etc.)
      const double slope = computeSlopeMsPerFrame();
      int desired = decidePreSteps(slope);
      if (aheadFr <= 0.0)
        desired = 0; // only pre-step when we're ahead of schedule

      // cap to remaining lookahead budget (e.g., 6–8 frames)
      int cap = settings_.predictiveLookaheadFrames -
                static_cast<int>(std::floor(std::max(0.0, aheadFr)));
      if (cap < 0)
        cap = 0;
      int n = std::min(desired, cap);

      for (int i = 0; i < n; ++i) {
        // Respect maxFrames to avoid overshooting
        if (settings_.maxFrames >= 0 && frame_ >= settings_.maxFrames)
          break;

        // Execute one extra frame right now
        uint64_t cs = Clock::now();
        executeFrame();
        uint64_t ce = Clock::now();

        // keep budget accounting for each pre-step
        double cm = Clock::to_ms(ce - cs);
        updateBudget(cm);

        // each pre-step moves the schedule forward by dt
        nextTarget_ += dtNs_;
        ++preSteps_;
      }
    }

    nextTarget_ += dtNs_;

    uint64_t now = Clock::now();
    if (now < nextTarget_) {
      uint64_t remain = nextTarget_ - now;
      if (remain > static_cast<uint64_t>(settings_.spinMicros) * 1000ULL)
        std::this_thread::sleep_for(
            std::chrono::nanoseconds(remain -
                                     static_cast<uint64_t>(settings_.spinMicros) *
                                         1000ULL));
      else {
      }
    }

    // reactive fallback
    if (settings_.adaptive) {
      logDrift();
      int64_t behind = static_cast<int64_t>(Clock::now()) -
                       static_cast<int64_t>(nextTarget_);
      if (behind > 0) {
        int extra =
            int((static_cast<double>(behind) / 1e9) / dt_.count());
        if (extra > settings_.maxCatchUp)
          extra = settings_.maxCatchUp;

        if (double(extra) >= settings_.adaptiveThresholdFrames)
          ++bursts_;

        extraSteps_ += extra;
        recoveredMs_ += extra * dt_.count() * 1000.0;

        for (int i = 0; i < extra; i++) {
          if (settings_.maxFrames >= 0 && frame_ >= settings_.maxFrames)
            break;

          uint64_t cs = Clock::now();
          executeFrame();
          uint64_t ce = Clock::now();
          double cm = Clock::to_ms(ce - cs);
          updateBudget(cm);
        }
      }
    } else
      logDrift();

    return !(settings_.maxFrames >= 0 && frame_ >= settings_.maxFrames);
  }

  // --- topo, phase execution, logging, budget -----------------------------
  std::vector<std::vector<std::size_t>> buildTopoLevels() {
    const std::size_t n = phases_.size();
    std::vector<int> indeg = indegree_;
    std::vector<char> enabled(n, 0);
    for (std::size_t i = 0; i < n; ++i)
      enabled[i] = phases_[i].enabled ? 1 : 0;

    std::vector<std::size_t> q;
    q.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
      if (enabled[i] && indeg[i] == 0)
        q.push_back(i);

    std::vector<std::vector<std::size_t>> levels;
    std::size_t head = 0;
    while (head < q.size()) {
      const std::size_t levelStart = head;
      while (head < q.size())
        ++head;
      using Diff = std::vector<std::size_t>::difference_type;
      std::vector<std::size_t> level(q.begin() + static_cast<Diff>(levelStart),
                                     q.begin() + static_cast<Diff>(head));
      if (level.empty())
        break;
      levels.push_back(level);

      for (auto u : level)
        for (auto v : succ_[u])
          if (enabled[v])
            if (--indeg[v] == 0)
              q.push_back(v);
    }
    for (std::size_t i = 0; i < n; ++i)
      if (enabled[i]) {
        bool listed = false;
        for (auto &L : levels)
          if (std::find(L.begin(), L.end(), i) != L.end()) {
            listed = true;
            break;
          }
        if (!listed)
          levels.push_back({i});
      }
    return levels;
  }

  void executePhase(std::size_t phaseIndex) {
    auto &ph = phases_[phaseIndex];
    if (!ph.enabled)
      return;
#ifdef SIM_USE_NUMA
    bool numaSet = false;
    if (ph.numaNode >= 0 && numa_available() != -1) {
      numa_set_preferred(ph.numaNode);
      numaSet = true;
    }
#endif

    bintrace_.log(bintrace::EV_PhaseBegin,
                  static_cast<std::uint32_t>(phaseIndex),
                  static_cast<std::uint64_t>(frame_));

    PROF_SCOPE(profiler_, "Phase:" + ph.name);
    for (auto &s : ph.serials)
      s.fn(s.ctx, frame_, dt_);

    // ---- Parallel range tasks with small-array cut-off ----
    if (workerCount_ > 1 && !ph.ranges.empty() && ph.elementCount > 0) {
      const std::size_t count = ph.elementCount;

      for (std::size_t tIdx = 0; tIdx < ph.ranges.size(); ++tIdx) {
        auto &rt = ph.ranges[tIdx];

        const std::size_t chunk = pickChunk(phaseIndex, count, rt.hint);
        const std::size_t totalChunks = (count + chunk - 1) / chunk;
        const std::size_t threads = workerCount_ + 1; // include main thread
        const std::size_t skew = threads ? (totalChunks % threads) : 0;

        ph.chosenChunk = chunk;
        ph.totalChunks = totalChunks;
        ph.chunkSkew = skew;
        LOG_TELEMETRY(logger_, "phase {}: chunk={} total={} skew={}", ph.name,
                      chunk, totalChunks, skew);
        if (profiler_) {
          profiler_->setCounter("phase." + ph.name + ".chunk",
                                static_cast<long double>(chunk));
          profiler_->setCounter("phase." + ph.name + ".total",
                                static_cast<long double>(totalChunks));
          profiler_->setCounter("phase." + ph.name + ".skew",
                                static_cast<long double>(skew));
        }

        // Inclusive thresholds: serialize when at/below the limits
        const bool tooFewElems = (settings_.minParallelElems > 0) &&
                                 (count <= settings_.minParallelElems);
        const bool tooFewChunks = (settings_.minParallelChunks > 1) &&
                                  (totalChunks <= settings_.minParallelChunks);

        if (tooFewElems || tooFewChunks) {
          // Run exactly once, serial, over the full range
          rt.fn(rt.ctx, 0, count, frame_, dt_);
          continue;
        }

        // parallel path…
        active_.fn = &rt;
        active_.totalChunks = totalChunks;
        active_.elementCount = count;
        active_.chunkSize = chunk;
        active_.frame = frame_;
        active_.dt = dt_;

        nextChunk_.store(0, std::memory_order_relaxed);
        remaining_.store(totalChunks, std::memory_order_release);
        dispatchToken_.fetch_add(1, std::memory_order_acq_rel);

        while (remaining_.load(std::memory_order_acquire) > 0) {
          std::size_t idx = nextChunk_.fetch_add(1, std::memory_order_relaxed);
          if (idx >= totalChunks) {
            std::this_thread::yield();
            continue;
          }
          std::size_t begin = idx * active_.chunkSize;
          std::size_t end = std::min(begin + active_.chunkSize, count);
          rt.fn(rt.ctx, begin, end, frame_, dt_);
          if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1)
            break;
        }
      }
    } else {
      for (auto &rt : ph.ranges)
        rt.fn(rt.ctx, 0, ph.elementCount, frame_, dt_);
    }

    for (auto &red : ph.reductions)
      red.fn(red.ctx, frame_, dt_);

    // Deterministic range reductions (parallel leaves + pairwise fold)
    for (auto &rr : ph.detRanges) {
      if (ph.elementCount == 0) {
        rr.sink(rr.sinkCtx, 0.0, nullptr, 0, frame_, dt_);
        continue;
      }
      const std::size_t count = ph.elementCount;
      const std::size_t leaf =
          std::max<std::size_t>(1, settings_.detReduceLeaf);
      const std::size_t chunk = std::min<std::size_t>(leaf, count);
      const std::size_t totalChunks = (count + chunk - 1) / chunk;
      const std::size_t threads = workerCount_ + 1;
      const std::size_t skew = threads ? (totalChunks % threads) : 0;

      ph.chosenChunk = chunk;
      ph.totalChunks = totalChunks;
      ph.chunkSkew = skew;
      LOG_TELEMETRY(logger_, "phase {}: chunk={} total={} skew={}", ph.name,
                    chunk, totalChunks, skew);
      if (profiler_) {
        profiler_->setCounter("phase." + ph.name + ".chunk",
                              static_cast<long double>(chunk));
        profiler_->setCounter("phase." + ph.name + ".total",
                              static_cast<long double>(totalChunks));
        profiler_->setCounter("phase." + ph.name + ".skew",
                              static_cast<long double>(skew));
      }

      ArenaResource res(frameArenas_->tls());
      std::pmr::vector<double> partials(&res);
      partials.resize(totalChunks, 0.0);
      std::pmr::vector<std::uint64_t> checksums(&res);
      checksums.resize(totalChunks, 0ull);

      struct LeafCtx {
        DetRangeReductionRef rr;
        std::pmr::vector<double> *partials;
        std::pmr::vector<std::uint64_t> *checksums;
        std::size_t chunk;
      };
      void *mem =
          frameArenas_->tls().allocate(sizeof(LeafCtx), alignof(LeafCtx));
      auto *leafCtx = new (mem) LeafCtx{rr, &partials, &checksums, chunk};

      auto leafThunk = [](void *c, std::size_t b, std::size_t e, std::int64_t f,
                          Seconds dt) {
        auto *LC = static_cast<LeafCtx *>(c);
        const std::size_t idx = b / LC->chunk;
        double v = LC->rr.leaf(LC->rr.leafCtx, b, e, f, dt);
        (*(LC->partials))[idx] = v;
        std::uint64_t bits;
        std::memcpy(&bits, &v, sizeof(v));
        std::uint64_t h = 1469598103934665603ull;
        h ^= bits;
        h *= 1099511628211ull;
        (*(LC->checksums))[idx] = h;
      };
      RangeFnRef leafRef{leafThunk, leafCtx};

      if (workerCount_ > 1 && pool_) {
        std::atomic<std::size_t> remaining(totalChunks);
        for (std::size_t idx = 0; idx < totalChunks; ++idx) {
          const std::size_t b = idx * chunk;
          const std::size_t e = std::min(b + chunk, count);
          pool_->submit([leafThunk, leafCtx, b, e, frame = frame_, dt = dt_,
                         &remaining] {
            leafThunk(leafCtx, b, e, frame, dt);
            remaining.fetch_sub(1, std::memory_order_acq_rel);
          });
        }
        while (remaining.load(std::memory_order_acquire) > 0)
          std::this_thread::yield();
      } else {
        for (std::size_t idx = 0; idx < totalChunks; ++idx) {
          const std::size_t b = idx * chunk;
          const std::size_t e = std::min(b + chunk, count);
          leafThunk(leafCtx, b, e, frame_, dt_);
        }
      }
      double total = simcore_det::pairwise_sum(partials);
      rr.sink(rr.sinkCtx, total, checksums.data(), totalChunks, frame_, dt_);
    }

    bintrace_.log(bintrace::EV_PhaseEnd, static_cast<std::uint32_t>(phaseIndex),
                  static_cast<std::uint64_t>(frame_));

#ifdef SIM_USE_NUMA
    if (numaSet)
      numa_set_preferred(-1);
#endif
  }

  void executeFrame() {
    if (frameArenas_)
      frameArenas_->beginFrame();
    if (graphDirty_) {
      levels_ = buildTopoLevels();
      graphDirty_ = false;
    }
    const auto &levels = levels_;
    for (const auto &level : levels) {
      if (level.empty())
        continue;
      if (!pool_ || level.size() == 1) {
        for (auto idx : level)
          executePhase(idx);
      } else {
        std::atomic<std::size_t> remaining(level.size());
        for (auto idx : level) {
          pool_->submit([this, idx, &remaining] {
            executePhase(idx);
            remaining.fetch_sub(1, std::memory_order_acq_rel);
          });
        }
        while (remaining.load(std::memory_order_acquire) != 0)
          std::this_thread::yield();
      }
    }
    ++frame_;
  }

  void logDrift() {
    if (settings_.driftLogInterval <= 0)
      return;
    if (frame_ % settings_.driftLogInterval)
      return;
    uint64_t now = Clock::now();
    double simT = static_cast<double>(frame_) * dt_.count();
    double realT = static_cast<double>(now - startReal_) / 1e9;
    lastDriftMs_ = (simT - realT) * 1000.0;
    if (logger_)
      LOG_INFO(logger_, "[DRIFT] frame={} drift={:.2f}ms", frame_,
               lastDriftMs_);
  }

  void recalcTiming() {
    subSteps_ =
        (settings_.hz > 1000.0) ? int(std::ceil(settings_.hz / 1000.0)) : 1;
    dt_ = Seconds{1.0 / settings_.hz};
    outerDt_ = dt_ * subSteps_;
    dtNs_ = static_cast<std::uint64_t>(dt_.count() * 1'000'000'000.0);
    startReal_ = Clock::now();
  }

public:
  void applyDegradeRung(int rung) {
    degradeRung_ = rung;
    if (rung >= 1)
      settings_.visualizers = false;
    if (rung >= 2 && subSteps_ > 1)
      --subSteps_;
    if (rung >= 3)
      settings_.broadphaseCoarse = true;
    if (rung >= 4)
      subSteps_ = 1;
    bintrace_.log(bintrace::EV_BudgetLadder, static_cast<std::uint32_t>(rung),
                  static_cast<std::uint64_t>(frame_));
  }

private:
  void updateBudget(double computeMs) {
    if (settings_.thermalMonitor && settings_.readPackageTemp) {
      double t = settings_.readPackageTemp();
      if (t >= settings_.thermalLimpCelsius && degradeRung_ < 4)
        applyDegradeRung(4);
    }
    if (!settings_.budgetMonitor || settings_.budgetWindow <= 0)
      return;

    if (!costWindow_.empty()) {
      if (costCount_ < costWindow_.size()) {
        costWindow_[costHead_] = computeMs;
        costSumMs_ += computeMs;
        ++costCount_;
        costHead_ = (costHead_ + 1) % costWindow_.size();
      } else {
        double &slot = costWindow_[costHead_];
        costSumMs_ += computeMs - slot;
        slot = computeMs;
        costHead_ = (costHead_ + 1) % costWindow_.size();
      }
    }

    const double periodMs = 1000.0 / settings_.hz;
    const double ratio = computeMs / periodMs;
    const double avgComputeMs =
        (costCount_ ? (costSumMs_ / double(costCount_)) : computeMs);
    const double avgRatio = avgComputeMs / periodMs;

    if (budgetCb_)
      budgetCb_(BudgetSample{computeMs, periodMs, ratio, avgComputeMs, avgRatio,
                             frame_});

    if (logger_) {
      if (ratio >= settings_.budgetCritRatio ||
          avgRatio >= settings_.budgetCritRatio) {
        LOG_WARN(logger_,
                 "[BUDGET] CRIT frame={} c={:.3f}ms r={:.2f} avg={:.3f}ms "
                 "avgR={:.2f}",
                 frame_, computeMs, ratio, avgComputeMs, avgRatio);
      } else if (ratio >= settings_.budgetWarnRatio ||
                 avgRatio >= settings_.budgetWarnRatio) {
        LOG_INFO(logger_,
                 "[BUDGET] WARN frame={} c={:.3f}ms r={:.2f} avg={:.3f}ms "
                 "avgR={:.2f}",
                 frame_, computeMs, ratio, avgComputeMs, avgRatio);
      }
    }

    const double ratioRef = std::max(ratio, avgRatio);
    const double t1 = settings_.budgetWarnRatio;
    const double t2 =
        (settings_.budgetWarnRatio + settings_.budgetCritRatio) * 0.5;
    const double t3 =
        std::max(settings_.budgetWarnRatio, settings_.budgetCritRatio - 0.02);
    if (degradeRung_ < 1 && ratioRef >= t1)
      applyDegradeRung(1);
    if (degradeRung_ < 2 && ratioRef >= t2)
      applyDegradeRung(2);
    if (degradeRung_ < 3 && ratioRef >= t3)
      applyDegradeRung(3);

    if (settings_.autoAdaptHz) {
      if (adaptCooldown_ > 0)
        --adaptCooldown_;
      if ((avgRatio >= settings_.budgetCritRatio ||
           ratio >= settings_.budgetCritRatio) &&
          adaptCooldown_ == 0) {
        const double newHz =
            std::max(settings_.minHz, settings_.hz * settings_.adaptDownFactor);
        if (newHz < settings_.hz) {
          if (logger_)
            LOG_WARN(logger_, "[BUDGET] autoAdaptHz: {} -> {}", settings_.hz,
                     newHz);
          settings_.hz = newHz;
          recalcTiming();
          adaptCooldown_ = settings_.adaptCooldownFrames;
        }
      }
    }
  }

  // ---- state ----
  Settings settings_{};
  std::uint64_t rngSeed_ = 0;
  std::vector<Phase> phases_;
  std::vector<std::vector<std::size_t>> succ_;
  std::vector<int> indegree_;
  bool graphDirty_ = false;
  std::vector<std::vector<std::size_t>> levels_;

  std::int64_t frame_ = 0;
  bool terminate_ = false;

  int subSteps_ = 1;
  Seconds dt_{1.0 / 500.0};
  Seconds outerDt_{dt_};
  std::uint64_t dtNs_{0};
  std::uint64_t nextTarget_{0};
  std::uint64_t startReal_{0};
  Seconds accumulator_{0};

  std::vector<std::thread> threads_;
  std::size_t workerCount_ = 0;
  std::atomic<bool> shutdown_{false};

  ActiveRange active_{};
  std::atomic<std::size_t> nextChunk_{0};
  std::atomic<std::size_t> remaining_{0};
  std::atomic<std::uint64_t> dispatchToken_{0};

  std::vector<double> costWindow_;
  std::size_t costHead_ = 0;
  std::size_t costCount_ = 0;
  double costSumMs_ = 0.0;
  int adaptCooldown_ = 0;
  int degradeRung_ = 0;
  std::function<void(const BudgetSample &)> budgetCb_;

  std::uint64_t deterministicHash_ = 0;
  double lastDriftMs_ = 0.0;
  int bursts_ = 0;
  int extraSteps_ = 0;
  double recoveredMs_ = 0.0;

  int preSteps_ = 0;
  double precoveredMs_ = 0.0;
  bool predictivePrimed_ = false;

  std::unique_ptr<FrameArenaPool> frameArenas_;
  std::unique_ptr<rt::FiberPool> fiberPool_;
  bintrace::Trace bintrace_;
  std::unordered_map<std::string, std::size_t> chunkCache_;
  std::string machineId_;

  Logger *logger_ = nullptr;
  Profiler *profiler_ = nullptr;
  WorkerPool *pool_ = nullptr;
};
