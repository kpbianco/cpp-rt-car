#pragma once
/* ===========================================================================
   SimCore ‑ lightweight simulation kernel (affinity‑/NUMA‑aware, adaptive
   step catch‑up, profiler hooks, deterministic reduction, unit‑test friendly)
   ======================================================================== */

#include <vector>
#include <thread>
#include <functional>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <string>
#include <cstring>
#include <iomanip>
#include <cassert>

#include "logger.hpp"
#include "profiler.hpp"

/* ---------- optional NUMA / core‑affinity helpers (Linux only) ----------- */
#ifdef __linux__
  #include <sched.h>
  #include <unistd.h>
  #ifdef SIM_USE_NUMA
    #include <numa.h>
  #endif
#endif

class SimCore {
public:
    using Seconds = std::chrono::duration<double>;
    using Clock   = std::chrono::steady_clock;

    /* ----------- public statistics for tests / UI ----------------------- */
    int          bursts()      const { return bursts_; }
    int          extraSteps()  const { return extraSteps_; }
    double       recoveredMs() const { return recoveredMs_; }
    double       lastDriftMs() const { return lastDriftMs_; }
    std::int64_t frame()       const { return frame_; }

    /* --------------------------- settings ------------------------------- */
    struct Settings {
        /* parallelism / NUMA */
        bool        pinThreads     = false;   ///< set explicit CPU affinity
        bool        compactNUMA    = false;   ///< pack threads per NUMA node

        /* timing */
        double      hz             = 500.0;   ///< simulation rate
        std::int64_t maxFrames     = 2500;    ///< –1 = infinite
        bool        adaptive       = false;   ///< catch‑up when lagging
        int         maxCatchUp     = 4;       ///< max extra frames per outer loop
        double      adaptiveThresholdFrames = 1.0;
        int         spinMicros     = 200;     ///< busy‑spin before wake

        /* job dispatch */
        std::size_t threads        = std::thread::hardware_concurrency();
        bool        mainHelps      = true;
        std::size_t chunkSize      = 256;

        /* logging / debug flags */
        bool        logChunks      = false;
        bool        logRangeTasks  = false;   ///< kept for old tests
        bool        logPhases      = false;
        int         driftLogInterval = 250;
    };

    /* ---------------- user task function types -------------------------- */
    using Subsystem     = std::function<void(std::int64_t, Seconds)>;
    using RangeTask     = std::function<void(std::size_t,std::size_t,
                                             std::int64_t,Seconds)>;
    using ReductionTask = std::function<void(std::int64_t,Seconds)>;

    struct Phase {
        std::string                name;
        std::vector<Subsystem>     serialSubs;
        std::vector<RangeTask>     rangeTasks;
        std::vector<ReductionTask> reductions;
        std::size_t                elementCount = 0;
        bool                       enabled      = true;
    };

    /* --------------------- construction --------------------------------- */
    SimCore() : SimCore(Settings{}) {}
    explicit SimCore(const Settings& s) { applySettings(s); initThreads(); }
    ~SimCore() { stopThreads(); }

    void setLogger  (Logger*   l) { logger_   = l; }
    void setProfiler(Profiler* p) { profiler_ = p; }

    /* ---------------- apply / update settings --------------------------- */
    void applySettings(const Settings& s) {
        settings_ = s;
        if (settings_.threads == 0) settings_.threads = 1;
        if (settings_.hz      <= 0) settings_.hz      = 1.0;
        recalcTiming();
        if (!threads_.empty() && settings_.threads != threadCount_) {
            stopThreads();
            initThreads();
        }
    }

    /* ---------------- phase & task registration ------------------------- */
    std::size_t addPhase(const std::string& name, std::size_t elems = 0) {
        phases_.push_back({name});
        phases_.back().elementCount = elems;
        return phases_.size() - 1;
    }
    void setPhaseElementCount(std::size_t p, std::size_t n) {
        phases_[p].elementCount = n;
    }
    void addSerialSubsystem(std::size_t p, Subsystem fn) {
        phases_[p].serialSubs.push_back(std::move(fn));
    }
    void addRangeTask(std::size_t p, RangeTask fn) {
        phases_[p].rangeTasks.push_back(std::move(fn));
    }
    /* compatibility alias (tests) */
    void addParallelRangeTask(std::size_t p, RangeTask fn) {
        addRangeTask(p, std::move(fn));
    }
    void addReductionTask(std::size_t p, ReductionTask fn) {
        phases_[p].reductions.push_back(std::move(fn));
    }

    void setDeterministicHash(std::uint64_t h) { deterministicHash_ = h; }
    std::uint64_t deterministicHash() const   { return deterministicHash_; }

    /* -------------------------- run loop -------------------------------- */
    void run() {
        startReal_  = Clock::now();
        nextTarget_ = startReal_;
        while (step()) { /* main outer loop */ }
    }

private:
    /* ======================= worker helpers ============================= */
    void initThreads() {
        stopThreads();
        threadCount_ = settings_.threads;
        shutdown_.store(false, std::memory_order_relaxed);

    #ifdef __linux__
        std::vector<int> cpuList;
        if (settings_.pinThreads) {
            int nCpu = ::sysconf(_SC_NPROCESSORS_ONLN);
            for (int c = 0; c < nCpu; ++c) cpuList.push_back(c);
        }
    #endif

        threads_.reserve(threadCount_);
        for (std::size_t i = 0; i < threadCount_; ++i) {
            threads_.emplace_back([this,i
    #ifdef __linux__
                                    ,cpuList
    #endif
                                   ] {
    #ifdef __linux__
                if (settings_.pinThreads && !cpuList.empty()) {
                    cpu_set_t m; CPU_ZERO(&m);
                    CPU_SET(cpuList[i % cpuList.size()], &m);
                    ::sched_setaffinity(0, sizeof(m), &m);
                }
    #endif
                workerLoop();
            });
        }
    }

    void stopThreads() {
        if (threads_.empty()) return;
        shutdown_.store(true, std::memory_order_release);
        token_.fetch_add(1, std::memory_order_acq_rel);     // wake workers
        for (auto& t : threads_) t.join();
        threads_.clear();
    }

    void workerLoop() {
        std::uint64_t local = token_.load(std::memory_order_acquire);
        while (true) {
            while (local == token_.load(std::memory_order_acquire) &&
                   !shutdown_.load(std::memory_order_acquire))
                std::this_thread::yield();
            if (shutdown_.load(std::memory_order_acquire)) break;
            local = token_.load(std::memory_order_acquire);
            processChunks();
        }
    }
    void processChunks() {
        for (;;) {
            std::size_t idx = nextIdx_.fetch_add(1, std::memory_order_relaxed);
            if (idx >= activeChunks_) break;
            std::size_t b = idx * chunkSize_;
            std::size_t e = std::min(b + chunkSize_, activeElems_);
            activeTask_(b, e, frame_, dt_);
            if (remain_.fetch_sub(1, std::memory_order_acq_rel) == 1) break;
        }
    }

    /* ======================= main loop helpers ========================== */
    bool step() {
        if (terminate_) return false;
        if (settings_.maxFrames >= 0 && frame_ >= settings_.maxFrames)
            return false;

        executeFrame();
        nextTarget_ += std::chrono::duration_cast<Clock::duration>(dt_);

        /* sleep‑then‑spin */
        for (;;) {
            auto now   = Clock::now();
            auto until = nextTarget_ - std::chrono::microseconds(settings_.spinMicros);
            if (now < until)
                std::this_thread::sleep_until(until);
            else if (now < nextTarget_)
                std::this_thread::yield();
            else
                break;
        }

        /* adaptive catch‑up */
        if (settings_.adaptive) {
            auto lag = Clock::now() - nextTarget_;
            if (lag.count() > 0) {
                int extra = int(std::chrono::duration<double>(lag).count() /
                                dt_.count());
                extra = std::clamp(extra, 0, settings_.maxCatchUp);
                if (extra >= settings_.adaptiveThresholdFrames) ++bursts_;
                extraSteps_  += extra;
                recoveredMs_ += extra * dt_.count() * 1000.0;
                for (int i = 0; i < extra; ++i) executeFrame();
            }
        }

        if (settings_.driftLogInterval > 0 &&
            frame_ % settings_.driftLogInterval == 0)
            logDrift();

        return !(settings_.maxFrames >= 0 && frame_ >= settings_.maxFrames);
    }

    /* one simulation frame */
    void executeFrame() {
        PROF_SCOPE(profiler_, "Frame");
        for (auto& ph : phases_) {
            if (!ph.enabled) continue;
            PROF_SCOPE(profiler_, "Phase:" + ph.name);

            for (auto& s : ph.serialSubs) s(frame_, dt_);

            /* parallel range tasks */
            if (!ph.rangeTasks.empty() && ph.elementCount > 0 &&
                threadCount_ > 1) {
                chunkSize_    = settings_.chunkSize ? settings_.chunkSize : 256;
                activeElems_  = ph.elementCount;

                for (std::size_t t = 0; t < ph.rangeTasks.size(); ++t) {
                    activeTask_   = ph.rangeTasks[t];
                    activeChunks_ = (activeElems_ + chunkSize_ - 1) / chunkSize_;

                    nextIdx_.store(0, std::memory_order_relaxed);
                    remain_.store(activeChunks_, std::memory_order_relaxed);
                    token_.fetch_add(1, std::memory_order_acq_rel); // wake workers

                    processChunks();            // main helps
                    while (remain_.load(std::memory_order_acquire) > 0)
                        std::this_thread::yield();
                }
            } else {
                for (auto& rt : ph.rangeTasks)
                    rt(0, ph.elementCount, frame_, dt_);
            }

            for (auto& r : ph.reductions) {
                PROF_SCOPE(profiler_, "Reduction:" + ph.name);
                r(frame_, dt_);
            }
        }
        ++frame_;
    }

    void logDrift() {
        double simT  = frame_ * dt_.count();
        double realT = std::chrono::duration<double>(Clock::now() - startReal_).count();
        lastDriftMs_ = (simT - realT) * 1000.0;
        LOG_INFO(logger_,
                 "[DRIFT] frame={} simT={:.3f}s realT={:.3f}s drift={:.2f}ms",
                 frame_, simT, realT, lastDriftMs_);
    }

    void recalcTiming() { dt_ = Seconds{1.0 / settings_.hz}; }

    /* ======================= data members =============================== */
    Settings                   settings_;
    std::vector<Phase>         phases_;

    /* frame state */
    std::int64_t               frame_      = 0;
    bool                       terminate_  = false;

    /* timing */
    Seconds                    dt_{1.0 / 500.0};
    Clock::time_point          startReal_;
    Clock::time_point          nextTarget_;

    /* worker dispatch */
    std::vector<std::thread>   threads_;
    std::size_t                threadCount_ = 0;
    std::atomic<bool>          shutdown_{false};

    RangeTask                  activeTask_;
    std::size_t                activeElems_  = 0;
    std::size_t                chunkSize_    = 0;
    std::size_t                activeChunks_ = 0;
    std::atomic<std::size_t>   nextIdx_{0};
    std::atomic<std::size_t>   remain_{0};
    std::atomic<std::uint64_t> token_{0};

    /* deterministic hash */
    std::uint64_t              deterministicHash_ = 0;

    /* adaptive & drift stats */
    double                     lastDriftMs_ = 0.0;
    int                        bursts_      = 0;
    int                        extraSteps_  = 0;
    double                     recoveredMs_ = 0.0;

    /* helpers */
    Logger*                    logger_   = nullptr;
    Profiler*                  profiler_ = nullptr;
};
