#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cmath>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <cassert>

/* --------------------------------------------------------------------------
   Lightweight work‑stealing style parallel_for over an index range.
   One WorkerPool lives for the life of SimCore.  Dispatch is via
   atomic fetch_add on an index counter; main thread can optionally help.
---------------------------------------------------------------------------- */
class WorkerPool {
public:
    using TaskFn = std::function<void(std::size_t)>;

    explicit WorkerPool(std::size_t nThreads = std::thread::hardware_concurrency(),
                        bool mainHelps = true)
        : mainHelps_(mainHelps)
    {
        if (nThreads == 0) nThreads = 1;
        threads_.reserve(nThreads);
        for (std::size_t i = 0; i < nThreads; ++i) {
            threads_.emplace_back([this]{ workerLoop(); });
        }
    }

    ~WorkerPool() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
            hasWork_ = true;        // wake everybody
        }
        cvStart_.notify_all();
        for (auto& t : threads_) t.join();
    }

    // parallel_for over [0, count); task(i) called once per i
    template<typename F>
    void parallel_for(std::size_t count, F&& task) {
        if (count == 0) return;

        {
            std::lock_guard<std::mutex> lk(m_);
            nextIndex_.store(0, std::memory_order_relaxed);
            total_   = count;
            finished_.store(0, std::memory_order_relaxed);
            task_ = [&task](std::size_t i){ task(i); };
            hasWork_ = true;
        }
        cvStart_.notify_all();

        if (mainHelps_) {
            runChunk();  // main thread participates
        }

        // wait for workers to finish
        std::unique_lock<std::mutex> lk(m_);
        cvDone_.wait(lk, [this]{
            return finished_.load(std::memory_order_acquire) == threads_.size();
        });
        hasWork_ = false;
    }

    std::size_t workerCount() const { return threads_.size(); }

private:
    void workerLoop() {
        for (;;) {
            // wait for work
            {
                std::unique_lock<std::mutex> lk(m_);
                cvStart_.wait(lk, [this]{ return hasWork_ || stop_; });
                if (stop_) return;
            }
            runChunk();  // do work
            // signal done
            if (finished_.fetch_add(1, std::memory_order_acq_rel) + 1 == threads_.size()) {
                std::lock_guard<std::mutex> lk(m_);
                cvDone_.notify_one();
            }
        }
    }

    void runChunk() {
        for (;;) {
            std::size_t i = nextIndex_.fetch_add(1, std::memory_order_relaxed);
            if (i >= total_) break;
            task_(i);
        }
    }

    // data ------------------------------------------------------------
    std::vector<std::thread> threads_;
    bool                     mainHelps_;   // main thread steals work too?

    std::mutex               m_;
    std::condition_variable  cvStart_;
    std::condition_variable  cvDone_;
    bool                     hasWork_ = false;
    bool                     stop_    = false;

    std::atomic<std::size_t> nextIndex_{0};
    std::size_t              total_   = 0;
    std::atomic<std::size_t> finished_{0};
    TaskFn                   task_;
};


/* --------------------------------------------------------------------------
   SimCore  --  fixed‑dt physics with sub‑stepping and scalable threading
---------------------------------------------------------------------------- */
class SimCore {
public:
    using Clock     = std::chrono::steady_clock;
    using Seconds   = std::chrono::duration<double>;
    using Subsystem = std::function<void(std::int64_t /*frame*/, Seconds /*dt*/)>;

    struct Settings {
        double        hz         = 400.0;   // physics micro‑step rate
        std::int64_t  maxFrames  = -1;      // -1 => endless
        bool          adaptive   = true;    // catch‑up when behind
        int           maxCatchUp = 4;       // max outer steps per catch‑up burst
        std::size_t   threads    = std::thread::hardware_concurrency(); // worker count
        bool          mainHelps  = true;    // let main thread process work
    };

    SimCore() : pool_(1) { applySettings(Settings{}); }
    explicit SimCore(const Settings& cfg)
        : pool_(cfg.threads ? cfg.threads : 1, cfg.mainHelps)
    {
        applySettings(cfg);
    }

    /* ---- configuration ---- */
    void applySettings(const Settings& s) {
        settings_ = s;

        // validate
        if (settings_.hz <= 0.0) settings_.hz = 1.0;
        if (settings_.maxCatchUp < 0) settings_.maxCatchUp = 0;
        if (settings_.maxFrames < -1) settings_.maxFrames = -1;
        if (settings_.threads == 0) settings_.threads = 1;

        hzAtom_.store(settings_.hz, std::memory_order_relaxed);

        recalcTimingFromHz();
    }

    void setHz(double hz) {
        settings_.hz = hz;
        hzAtom_.store(hz, std::memory_order_relaxed);
        recalcTimingFromHz();
    }

    void setMaxFrames(std::int64_t mf) { settings_.maxFrames = (mf < -1) ? -1 : mf; }
    void setAdaptive(bool en)          { settings_.adaptive = en; }
    void setMaxCatchUp(int m)          { settings_.maxCatchUp = (m < 0) ? 0 : m; }

    double hz()        const { return hzAtom_.load(std::memory_order_relaxed); }
    double dtSeconds() const { return dtMicro_.count(); }

    void addSubsystem(Subsystem f) { subsystems_.emplace_back(std::move(f)); }

    std::int64_t currentFrame() const { return frame_; }
    void requestExit()                 { terminate_ = true; }

    /* ---- run loop ---- */
    void run() {
        auto nextOuter = Clock::now();
        const auto minSleep = std::chrono::milliseconds(1);

        while (advanceSimulation()) {

            nextOuter += outerDtChrono_;

            if (outerDtChrono_ > minSleep) {
                std::this_thread::sleep_until(nextOuter);
            } else {
                // sub‑ms outer step: soft busy‑wait
                while (Clock::now() < nextOuter) std::this_thread::yield();
            }

            if (settings_.adaptive) {
                auto lag = Clock::now() - nextOuter;
                int lagOuters = static_cast<int>(
                    std::chrono::duration<double>(lag).count() / outerDt_.count());
                lagOuters = std::clamp(lagOuters, 0, settings_.maxCatchUp);
                for (int i = 0; i < lagOuters; ++i) advanceSimulation();
                nextOuter = Clock::now();
            }
        }
    }

private:
    /* ---- recompute derived timing from Hz ---- */
    void recalcTimingFromHz() noexcept {
        const double hz = settings_.hz;
        subSteps_  = (hz > 1000.0) ? static_cast<int>(std::ceil(hz / 1000.0)) : 1;
        dtMicro_   = Seconds{1.0 / hz};
        outerDt_   = dtMicro_ * subSteps_;
        outerDtChrono_ = std::chrono::duration_cast<Clock::duration>(outerDt_);
    }

    /* ---- do one outer step (may contain many micro‑steps) ---- */
    bool advanceSimulation() {
        if (terminate_ ||
            (settings_.maxFrames >= 0 && frame_ >= settings_.maxFrames))
            return false;

        for (int i = 0; i < subSteps_; ++i) {
            if (pool_.workerCount() > 1 && subsystems_.size() > 1) {
                // parallel dispatch: each subsystem index processed once
                pool_.parallel_for(subsystems_.size(), [this](std::size_t idx){
                    subsystems_[idx](frame_, dtMicro_);
                });
            } else {
                // serial fallback
                for (auto& f : subsystems_) f(frame_, dtMicro_);
            }
            ++frame_;
        }
        return true;
    }

    /* ---- data ---- */
    Settings                   settings_{};
    std::atomic<double>        hzAtom_{settings_.hz};

    WorkerPool                 pool_;           // must be constructed before timings? we handle in ctor

    std::vector<Subsystem>     subsystems_;
    std::int64_t               frame_      = 0;
    bool                       terminate_  = false;

    // timing derived from Hz
    int                        subSteps_   = 1;
    Seconds                    dtMicro_{1.0 / settings_.hz};
    Seconds                    outerDt_{dtMicro_};
    Clock::duration            outerDtChrono_{};
};
