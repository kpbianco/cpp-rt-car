#pragma once
#include <vector>
#include <thread>
#include <functional>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cassert>
#include <algorithm>
#include <string>
#include <cstring>
#include <iomanip>

#include "logger.hpp"
#include "profiler.hpp"
#include "worker_pool.hpp"   // for optional phase-level parallelism

#include <sched.h>              // CPU affinity (Linux/POSIX)
#ifdef __linux__
#  include <unistd.h>
#  include <sys/syscall.h>
#  include <sys/types.h>
#  ifdef SIM_USE_NUMA
#    include <numa.h>           // libnuma (optional)
#  endif
#endif

// ---- Small internal utility: deterministic pairwise reduction (fixed order) ----
namespace simcore_det {
    template <class T>
    T pairwise_sum(const T* data, std::size_t n)
    {
        if (n == 0) return T(0);
        std::vector<T> level(data, data + n);
        while (level.size() > 1) {
            const std::size_t m      = level.size();
            const std::size_t pairs  = m / 2;
            const std::size_t remain = m % 2;
            std::vector<T> next;
            next.resize(pairs + remain);
            for (std::size_t i = 0; i < pairs; ++i) {
                const std::size_t a = 2 * i;
                const std::size_t b = a + 1;
                next[i] = level[a] + level[b];
            }
            if (remain) next[pairs] = level[m - 1];
            level.swap(next);
        }
        return level[0];
    }
} // namespace simcore_det

class SimCore {
public:
    using Seconds = std::chrono::duration<double>;
    using Clock   = std::chrono::steady_clock;

    int    bursts()      const { return bursts_; }
    int    extraSteps()  const { return extraSteps_; }
    double recoveredMs() const { return recoveredMs_; }

    struct Settings {
        bool          pinThreads   = false;     // optional CPU affinity
        bool          compactNUMA  = false;     // pack workers per NUMA node
        double        hz         = 500.0;
        std::int64_t  maxFrames  = 2500;
        bool          adaptive   = false;
        int           maxCatchUp = 4;
        std::size_t   threads    = std::thread::hardware_concurrency();
        bool          mainHelps  = true;
        std::size_t   chunkSize  = 256;
        int           driftLogInterval = 250;
        int           spinMicros = 200;
        bool          logPhases = false;
        bool          logRangeTasks = false;
        double        adaptiveThresholdFrames = 1.0;  // count burst if > this many frames
        bool          logChunks = false;              // fine-grained task logging

        // Chunk auto-tuning (for normal parallel range tasks)
        bool          autoTuneChunks = true;          // enable or disable auto chunking
        int           targetChunksPerThread = 2;      // chunks per worker
        std::size_t   minChunk = 1;                   // lower clamp
        std::size_t   maxChunk = 8192;                // upper clamp

        // NEW: deterministic reductions use a fixed, thread-independent leaf size.
        std::size_t   detReduceLeaf = 1024;           // elements per leaf for deterministic reductions
    };

    using Subsystem     = std::function<void(std::int64_t frame, Seconds dt)>;
    using RangeTask     = std::function<void(std::size_t begin, std::size_t end,
                                             std::int64_t frame, Seconds dt)>;
    using ReductionTask = std::function<void(std::int64_t frame, Seconds dt)>;

    // Deterministic range-reduction types
    using RangeReduceFn = std::function<double(std::size_t begin, std::size_t end,
                                               std::int64_t frame, Seconds dt)>;
    using ReduceSinkFn  = std::function<void(double total, std::int64_t frame, Seconds dt)>;
    struct RangeReduce { RangeReduceFn fn; ReduceSinkFn sink; };

    struct Phase {
        std::string                name;
        std::vector<Subsystem>     serialSubsystems;
        std::vector<RangeTask>     parallelRangeTasks;
        std::vector<ReductionTask> reductions;
        std::vector<RangeReduce>   detRangeReductions;   // NEW
        std::size_t                elementCount = 0;
        bool                       enabled      = true;
    };

    SimCore() : SimCore(Settings{}) {}
    explicit SimCore(const Settings& s) { applySettings(s); initThreads(); }
    ~SimCore() { stopThreads(); }

    void setLogger(Logger* l)    { logger_ = l; }
    void setProfiler(Profiler* p){ profiler_ = p; }

    // Optional: provide a WorkerPool to enable parallel phase execution per topo level
    void setWorkerPool(WorkerPool* pool) { pool_ = pool; }

    void applySettings(const Settings& s) {
        settings_ = s;
        if (settings_.hz <= 0.0) settings_.hz = 1.0;
        if (settings_.threads == 0) settings_.threads = 1;
        if (settings_.maxCatchUp < 0) settings_.maxCatchUp = 0;
        if (settings_.detReduceLeaf == 0) settings_.detReduceLeaf = 1024;
        recalcTiming();
        if (!threads_.empty() && settings_.threads != threadCount_) {
            stopThreads();
            initThreads();
        }
        LOG_INFO(logger_,
                 "Config hz={} maxFrames={} threads={} mainHelps={} chunk={} adaptive={} driftInterval={} spinMicros={} detLeaf={}",
                 settings_.hz, settings_.maxFrames, settings_.threads,
                 settings_.mainHelps, settings_.chunkSize, settings_.adaptive,
                 settings_.driftLogInterval, settings_.spinMicros, settings_.detReduceLeaf);
    }

#ifdef __linux__
    inline bool set_affinity(std::size_t cpu)
    {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(cpu, &mask);
        return ::sched_setaffinity(0, sizeof(mask), &mask) == 0;
    }
#endif

    std::size_t addPhase(const std::string& name, std::size_t elemCount = 0) {
        phases_.emplace_back(Phase{name, {}, {}, {}, {}, elemCount, true});
        // grow dependency structures to match
        succ_.push_back({});
        indegree_.push_back(0);
        graphDirty_ = true;

        LOG_DEBUG(logger_, "AddPhase '{}' elemCount={}", name, elemCount);
        return phases_.size() - 1;
    }

    void setPhaseElementCount(std::size_t phaseIndex, std::size_t count) {
        phases_[phaseIndex].elementCount = count;
        LOG_DEBUG(logger_, "Phase '{}' set elementCount={}",
                  phases_[phaseIndex].name, count);
    }
    void addSerialSubsystem(std::size_t phaseIndex, Subsystem fn) {
        phases_[phaseIndex].serialSubsystems.push_back(std::move(fn));
        LOG_TRACE(logger_, "Add serial subsystem to phase '{}'",
                  phases_[phaseIndex].name);
    }
    void addParallelRangeTask(std::size_t phaseIndex, RangeTask fn) {
        phases_[phaseIndex].parallelRangeTasks.push_back(std::move(fn));
        LOG_TRACE(logger_, "Add parallel range task to phase '{}'",
                  phases_[phaseIndex].name);
    }
    void addReductionTask(std::size_t phaseIndex, ReductionTask fn) {
        phases_[phaseIndex].reductions.push_back(std::move(fn));
        LOG_TRACE(logger_, "Add reduction task to phase '{}'",
                  phases_[phaseIndex].name);
    }

    // Deterministic range-reduction registration
    void addDeterministicRangeReduction(std::size_t phaseIndex,
                                        RangeReduceFn rangeFn,
                                        ReduceSinkFn  sinkFn)
    {
        phases_[phaseIndex].detRangeReductions.push_back(
            RangeReduce{std::move(rangeFn), std::move(sinkFn)});
        LOG_TRACE(logger_, "Add deterministic range reduction to phase '{}'",
                  phases_[phaseIndex].name);
    }

    // --- Phase dependency graph API ---
    // "before -> after" (after depends on before). Returns false on duplicate/self/OOO index.
    bool addDependency(std::size_t before, std::size_t after) {
        if (before >= phases_.size() || after >= phases_.size() || before == after) return false;
        // avoid duplicate edges
        auto& s = succ_[before];
        if (std::find(s.begin(), s.end(), after) != s.end()) return false;
        s.push_back(after);
        indegree_[after] += 1;
        graphDirty_ = true;
        LOG_DEBUG(logger_, "Dep: '{}' -> '{}'", phases_[before].name, phases_[after].name);
        return true;
    }

    void setDeterministicHash(std::uint64_t h) { deterministicHash_ = h; }
    std::uint64_t deterministicHash() const { return deterministicHash_; }

    void requestExit() { terminate_ = true; }
    std::int64_t frame() const { return frame_; }
    double dtSeconds()  const { return dt_.count(); }
    double lastDriftMs() const { return lastDriftMs_; }

    void run() {
        LOG_INFO(logger_, "Run loop start (accumulator)");
        startReal_ = Clock::now();
        accumulator_ = Seconds{0};
        nextTarget_ = startReal_;
        while (step()) { /* loop */ }
        LOG_INFO(logger_, "Run loop end frame={}", frame_);
    }

private:
    struct ActiveRange {
        RangeTask*           task         = nullptr;         // normal range task
        RangeReduceFn*       reduceTask   = nullptr;         // range->double (for det reductions)
        std::vector<double>* partials     = nullptr;         // per-chunk partials
        std::size_t          totalChunks  = 0;
        std::size_t          elementCount = 0;
        std::size_t          chunkSize    = 0;
        std::int64_t         frame        = 0;
        Seconds              dt{};
    };

    // ------------------ thread helpers (range-level) ------------------
    void initThreads()
    {
        stopThreads();
        threadCount_ = settings_.threads;
        shutdown_.store(false, std::memory_order_relaxed);

#ifdef __linux__
        std::vector<int> cpuList;
        if (settings_.pinThreads) {
            const int nCpus = ::sysconf(_SC_NPROCESSORS_ONLN);
#   ifdef SIM_USE_NUMA
            if (settings_.compactNUMA && numa_available() != -1) {
                const int maxNode = numa_max_node();
                for (int node = 0; node <= maxNode; ++node) {
                    struct bitmask *bm = numa_allocate_cpumask();
                    numa_node_to_cpus(node, bm);
                    for (int c = 0; c < nCpus; ++c)
                        if (numa_bitmask_isbitset(bm, c)) cpuList.push_back(c);
                    numa_free_cpumask(bm);
                }
            } else
#   endif
            {
                for (int c = 0; c < nCpus; ++c) cpuList.push_back(c);
            }
        }
#endif

        threads_.reserve(threadCount_);
        for (std::size_t i = 0; i < threadCount_; ++i) {
            threads_.emplace_back([this, i,
#ifdef __linux__
                                   cpuList
#endif
                                   ] {
#ifdef __linux__
                if (settings_.pinThreads && !cpuList.empty())
                    set_affinity(std::size_t(cpuList[i % cpuList.size()]));
#endif
                workerLoop();
            });
        }
        LOG_INFO(logger_, "Threads initialised count={} pin={} compactNUMA={}",
                 threadCount_, settings_.pinThreads, settings_.compactNUMA);
    }

    void stopThreads() {
        if (threads_.empty()) return;
        shutdown_.store(true, std::memory_order_release);
        dispatchToken_.fetch_add(1, std::memory_order_acq_rel);
        for (auto& t : threads_) t.join();
        threads_.clear();
        LOG_INFO(logger_, "Threads stopped");
    }

    void workerLoop() {
        std::uint64_t localToken = dispatchToken_.load(std::memory_order_acquire);
        for (;;) {
            while (localToken == dispatchToken_.load(std::memory_order_acquire) &&
                   !shutdown_.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (shutdown_.load(std::memory_order_acquire)) break;
            localToken = dispatchToken_.load(std::memory_order_acquire);
            processActiveRange();
        }
    }

    void processActiveRange()
    {
        for (;;)
        {
            std::size_t idx = nextChunk_.fetch_add(1, std::memory_order_relaxed);
            if (idx >= active_.totalChunks) break;
            std::size_t b = idx * active_.chunkSize;
            std::size_t e = std::min(b + active_.chunkSize, active_.elementCount);

            if (settings_.logChunks)
                LOG_TRACE(logger_, "ChunkStart tid={} idx={} b={} e={}",
                          std::this_thread::get_id(), idx, b, e);

            if (active_.reduceTask) {
                double v = (*active_.reduceTask)(b, e, active_.frame, active_.dt);
                // fixed index write ensures determinism
                (*active_.partials)[idx] = v;
            } else if (active_.task) {
                (*active_.task)(b, e, active_.frame, active_.dt);
            }

            if (settings_.logChunks)
                LOG_TRACE(logger_, "ChunkDone  tid={} idx={}", std::this_thread::get_id(), idx);

            if (remaining_.fetch_sub(1,std::memory_order_acq_rel) == 1) break;
        }
    }

    std::size_t pickChunk(std::size_t elems) const {
        if (!settings_.autoTuneChunks) {
            return std::max<std::size_t>(1, settings_.chunkSize ? settings_.chunkSize : 256);
        }
        // Aim for: totalChunks ~= threads * targetChunksPerThread
        const std::size_t threads = std::max<std::size_t>(1, threadCount_);
        const std::size_t targetChunks =
            std::max<std::size_t>(1, threads * static_cast<std::size_t>(std::max(1, settings_.targetChunksPerThread)));

        std::size_t chunk = (elems + targetChunks - 1) / targetChunks; // ceil
        chunk = std::clamp(chunk, settings_.minChunk, settings_.maxChunk);
        if (chunk > elems) chunk = elems;        // never larger than the array
        if (chunk == 0)       chunk = 1;         // never 0
        return chunk;
    }

    // ------------------ main step / frame ------------------
    bool step()
    {
        if (terminate_) return false;
        if (settings_.maxFrames >= 0 && frame_ >= settings_.maxFrames) return false;

        executeFrame();

        nextTarget_ += std::chrono::duration_cast<Clock::duration>(dt_);

        // sleep/spin to aim for cadence
        auto now = Clock::now();
        if (now < nextTarget_) {
            auto remain = std::chrono::duration_cast<std::chrono::microseconds>(nextTarget_ - now);
            if (remain.count() > settings_.spinMicros)
                std::this_thread::sleep_for(remain - std::chrono::microseconds(settings_.spinMicros));
            else
                ; /* busy */
        }

        if (settings_.adaptive)
        {
            logDrift();
            auto behind = Clock::now() - nextTarget_;
            if (behind.count() > 0)
            {
                int extra = int(std::chrono::duration<double>(behind).count()
                                / dt_.count());
                if (extra > settings_.maxCatchUp) extra = settings_.maxCatchUp;

                if (double(extra) >= settings_.adaptiveThresholdFrames)
                    ++bursts_;

                extraSteps_   += extra;
                recoveredMs_  += extra * dt_.count() * 1000.0;

                for (int i=0;i<extra;i++)
                {
                    if (settings_.maxFrames >= 0 && frame_ >= settings_.maxFrames)
                        break;
                    executeFrame();
                }
            }
        }
        else
            logDrift();

        return !(settings_.maxFrames >= 0 && frame_ >= settings_.maxFrames);
    }

    // Build topo levels (Kahn’s algorithm levels) each frame (cheap; phases are few).
    // Only enabled phases participate.
    std::vector<std::vector<std::size_t>> buildTopoLevels() {
        const std::size_t n = phases_.size();
        std::vector<int> indeg = indegree_;            // copy base
        std::vector<char> enabled(n, 0);
        for (std::size_t i=0; i<n; ++i) enabled[i] = phases_[i].enabled ? 1 : 0;

        // queue initial zeros
        std::vector<std::size_t> q;
        q.reserve(n);
        for (std::size_t i=0;i<n;++i)
            if (enabled[i] && indeg[i]==0) q.push_back(i);

        std::vector<std::vector<std::size_t>> levels;
        std::size_t head = 0;
        while (head < q.size()) {
            const std::size_t levelStart = head;
            while (head < q.size()) { ++head; }
            std::vector<std::size_t> level(q.begin() + levelStart, q.begin() + head);
            if (level.empty()) break;
            levels.push_back(level);

            for (auto u : level) {
                for (auto v : succ_[u]) {
                    if (!enabled[v]) continue;
                    if (--indeg[v] == 0) q.push_back(v);
                }
            }
        }

        // Cycle fallback: append any remaining enabled nodes in index order
        for (std::size_t i=0;i<n;++i) {
            if (enabled[i]) {
                bool alreadyListed = false;
                for (auto& L : levels)
                    if (std::find(L.begin(), L.end(), i) != L.end()) { alreadyListed = true; break; }
                if (!alreadyListed) levels.push_back({i});
            }
        }
        return levels;
    }

    // Factor: execute a single phase’s content (serial, range tasks, reductions)
    void executePhase(std::size_t phaseIndex) {
        auto& ph = phases_[phaseIndex];
        if (!ph.enabled) return;

        if (settings_.logPhases)
            LOG_DEBUG(logger_, "PhaseBegin '{}' frame={}", ph.name, frame_);
        PROF_SCOPE(profiler_, "Phase:" + ph.name);

        for (auto& sub : ph.serialSubsystems)
            sub(frame_, dt_);

        // Parallel range tasks (no reduction)
        if (threadCount_ > 1 &&
            !ph.parallelRangeTasks.empty() &&
            ph.elementCount > 0) {
            std::size_t count = ph.elementCount;
            for (std::size_t tIdx=0; tIdx < ph.parallelRangeTasks.size(); ++tIdx) {
                auto& rt = ph.parallelRangeTasks[tIdx];
                std::size_t chunk = pickChunk(count);
                std::size_t totalChunks = (count + chunk - 1)/chunk;

                active_.task         = &rt;
                active_.reduceTask   = nullptr;
                active_.partials     = nullptr;
                active_.totalChunks  = totalChunks;
                active_.elementCount = count;
                active_.chunkSize    = chunk;
                active_.frame        = frame_;
                active_.dt           = dt_;

                nextChunk_.store(0, std::memory_order_relaxed);
                remaining_.store(totalChunks, std::memory_order_release);
                dispatchToken_.fetch_add(1, std::memory_order_acq_rel);

                while (remaining_.load(std::memory_order_acquire) > 0) {
                    std::size_t idx = nextChunk_.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= totalChunks) {
                        std::this_thread::yield();
                        continue;
                    }
                    std::size_t begin = idx * chunk;
                    std::size_t end   = std::min(begin + chunk, count);
                    PROF_SCOPE(profiler_, "RangeTask:" + ph.name + ":" + std::to_string(tIdx));
                    rt(begin, end, frame_, dt_);
                    if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1)
                        break;
                }
            }
        } else {
            for (auto& rt : ph.parallelRangeTasks) {
                PROF_SCOPE(profiler_, "RangeTask:" + ph.name + ":S");
                rt(0, ph.elementCount, frame_, dt_);
            }
        }

        // Deterministic range-reductions (per-chunk partials + fixed-order fold)
        // Deterministic range-reductions (per-chunk partials + fixed-order fold)
for (auto& rr : ph.detRangeReductions) {
    if (ph.elementCount == 0) {
        rr.sink(0.0, frame_, dt_);
        continue;
    }

    const std::size_t count = ph.elementCount;

    // Use a fixed, thread-independent leaf size ALWAYS (even single-thread).
    const std::size_t leaf  = std::max<std::size_t>(1, settings_.detReduceLeaf);
    const std::size_t chunk = std::min<std::size_t>(leaf, count);
    const std::size_t totalChunks = (count + chunk - 1) / chunk;

    std::vector<double> partials(totalChunks, 0.0);

    if (threadCount_ > 1) {
        // Parallel: workers fill fixed-index partials[ idx ]
        active_.task         = nullptr;
        active_.reduceTask   = &rr.fn;
        active_.partials     = &partials;
        active_.totalChunks  = totalChunks;
        active_.elementCount = count;
        active_.chunkSize    = chunk;
        active_.frame        = frame_;
        active_.dt           = dt_;

        nextChunk_.store(0, std::memory_order_relaxed);
        remaining_.store(totalChunks, std::memory_order_release);
        dispatchToken_.fetch_add(1, std::memory_order_acq_rel);

        // main thread helps drain
        while (remaining_.load(std::memory_order_acquire) > 0) {
            std::size_t idx = nextChunk_.fetch_add(1, std::memory_order_relaxed);
            if (idx >= totalChunks) { std::this_thread::yield(); continue; }
            const std::size_t b = idx * chunk;
            const std::size_t e = std::min(b + chunk, count);
            double v = rr.fn(b, e, frame_, dt_);
            partials[idx] = v;
            if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1)
                break;
        }
    } else {
        // Single-thread: same chunking as parallel path for identical order
        for (std::size_t idx = 0; idx < totalChunks; ++idx) {
            const std::size_t b = idx * chunk;
            const std::size_t e = std::min(b + chunk, count);
            partials[idx] = rr.fn(b, e, frame_, dt_);
        }
    }

    // Fixed-order pairwise fold -> bit-stable across thread counts
    double total = simcore_det::pairwise_sum(partials.data(), partials.size());
    rr.sink(total, frame_, dt_);
}

        for (auto& red : ph.reductions) {
            PROF_SCOPE(profiler_, "Reduction:" + ph.name);
            red(frame_, dt_);
        }

        if (settings_.logPhases)
            LOG_DEBUG(logger_, "PhaseEnd   '{}' frame={}", ph.name, frame_);
    }

    void executeFrame() {
        PROF_SCOPE(profiler_, "Frame");

        // compute topological levels (parallelizable “frontiers”)
        auto levels = buildTopoLevels();

        for (const auto& level : levels) {
            if (level.empty()) continue;

            if (!pool_ || level.size() == 1) {
                // no pool or only one node: run sequentially
                for (auto idx : level) executePhase(idx);
            } else {
                // schedule this level’s phases in parallel using WorkerPool
                std::atomic<std::size_t> remaining(level.size());
                for (auto idx : level) {
                    pool_->enqueue([this, idx, &remaining]{
                        executePhase(idx);
                        remaining.fetch_sub(1, std::memory_order_acq_rel);
                    });
                }
                // wait for the level to finish
                while (remaining.load(std::memory_order_acquire) != 0) {
                    std::this_thread::yield();
                }
            }
        }

        ++frame_;
        if ((frame_ & 0x3FF) == 0)
            LOG_INFO(logger_, "Progress frame={}", frame_);
    }

    void logDrift() {
        if (settings_.driftLogInterval <= 0) return;
        if (frame_ % settings_.driftLogInterval) return;
        auto now = Clock::now();
        double simT  = static_cast<double>(frame_) * dt_.count();
        double realT = std::chrono::duration<double>(now - startReal_).count();
        double driftMs = (simT - realT) * 1000.0;
        lastDriftMs_ = driftMs;
        LOG_INFO(logger_, "[DRIFT] frame={} simT={:.3f}s realT={:.3f}s drift={:.2f}ms",
                 frame_, simT, realT, driftMs);
    }

    void recalcTiming() {
        subSteps_  = (settings_.hz > 1000.0)
                   ? int(std::ceil(settings_.hz / 1000.0))
                   : 1;
        dt_        = Seconds{1.0 / settings_.hz};
        outerDt_   = dt_ * subSteps_;
        outerDtChrono_ = std::chrono::duration_cast<Clock::duration>(outerDt_);
        startReal_ = Clock::now();
    }

    // ------------------ state ------------------
    Settings               settings_{};
    std::vector<Phase>     phases_;
    // dependency graph (static across frames)
    std::vector<std::vector<std::size_t>> succ_;     // adjacency list
    std::vector<int>                         indegree_;
    bool                                     graphDirty_ = false; // reserved for future caching

    std::int64_t           frame_      = 0;
    bool                   terminate_  = false;

    int                    subSteps_   = 1;
    Seconds                dt_{1.0 / 500.0};
    Seconds                outerDt_{dt_};
    Clock::duration        outerDtChrono_{};
    Clock::time_point      nextTarget_{};
    Clock::time_point      startReal_{};
    Seconds                accumulator_{0};

    // internal thread team for range tasks
    std::vector<std::thread> threads_;
    std::size_t              threadCount_ = 0;
    std::atomic<bool>        shutdown_{false};

    ActiveRange                active_{};
    std::atomic<std::size_t>   nextChunk_{0};
    std::atomic<std::size_t>   remaining_{0};
    std::atomic<std::uint64_t> dispatchToken_{0};

    std::uint64_t            deterministicHash_ = 0;
    double                   lastDriftMs_ = 0.0;
    int                      bursts_        = 0;
    int                      extraSteps_    = 0;
    double                   recoveredMs_   = 0.0;

    Logger*     logger_   = nullptr;
    Profiler*   profiler_ = nullptr;
    WorkerPool* pool_     = nullptr;   // optional: for phase-level parallelism
};
