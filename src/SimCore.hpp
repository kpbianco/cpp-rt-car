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
#include "logger.hpp"
#include "profiler.hpp"

class SimCore {
public:
    using Seconds = std::chrono::duration<double>;
    using Clock   = std::chrono::steady_clock;

    struct Settings {
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
    };

    using Subsystem     = std::function<void(std::int64_t frame, Seconds dt)>;
    using RangeTask     = std::function<void(std::size_t begin, std::size_t end,
                                             std::int64_t frame, Seconds dt)>;
    using ReductionTask = std::function<void(std::int64_t frame, Seconds dt)>;

    struct Phase {
        std::string                name;
        std::vector<Subsystem>     serialSubsystems;
        std::vector<RangeTask>     parallelRangeTasks;
        std::vector<ReductionTask> reductions;
        std::size_t                elementCount = 0;
        bool                       enabled      = true;
    };

    SimCore() : SimCore(Settings{}) {}
    explicit SimCore(const Settings& s) { applySettings(s); initThreads(); }
    ~SimCore() { stopThreads(); }

    void setLogger(Logger* l)    { logger_ = l; }
    void setProfiler(Profiler* p){ profiler_ = p; }

    void applySettings(const Settings& s) {
        settings_ = s;
        if (settings_.hz <= 0.0) settings_.hz = 1.0;
        if (settings_.threads == 0) settings_.threads = 1;
        if (settings_.maxCatchUp < 0) settings_.maxCatchUp = 0;
        recalcTiming();
        if (!threads_.empty() && settings_.threads != threadCount_) {
            stopThreads();
            initThreads();
        }
        LOG_INFO(logger_,
                 "Config hz={} maxFrames={} threads={} mainHelps={} chunk={} adaptive={} driftInterval={} spinMicros={}",
                 settings_.hz, settings_.maxFrames, settings_.threads,
                 settings_.mainHelps, settings_.chunkSize, settings_.adaptive,
                 settings_.driftLogInterval, settings_.spinMicros);
    }

    std::size_t addPhase(const std::string& name, std::size_t elemCount = 0) {
        phases_.emplace_back(Phase{name, {}, {}, {}, elemCount, true});
        LOG_DEBUG(logger_, "AddPhase '{}' elemCount={}", name, elemCount);
        return phases_.size()-1;
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

    void setDeterministicHash(std::uint64_t h) { deterministicHash_ = h; }
    std::uint64_t deterministicHash() const { return deterministicHash_; }

    void requestExit() { terminate_ = true; }
    std::int64_t frame() const { return frame_; }
    double dtSeconds()  const { return dtMicro_.count(); }
    double lastDriftMs() const { return lastDriftMs_; }

    void run() {
        LOG_INFO(logger_, "Run loop start (accumulator)");
        startReal_ = Clock::now();
        accumulator_ = Seconds{0};
        nextFrameTarget_ = startReal_;
        while (advance()) { /* loop */ }
        LOG_INFO(logger_, "Run loop end frame={}", frame_);
    }

private:
    struct ActiveRange {
        RangeTask*   task         = nullptr;
        std::size_t  totalChunks  = 0;
        std::size_t  elementCount = 0;
        std::size_t  chunkSize    = 0;
        std::int64_t frame        = 0;
        Seconds      dt{};
    };

    void initThreads() {
        stopThreads();
        threadCount_ = settings_.threads;
        shutdown_.store(false, std::memory_order_relaxed);
        threads_.reserve(threadCount_);
        for (std::size_t i=0;i<threadCount_;++i)
            threads_.emplace_back([this]{ workerLoop(); });
        LOG_INFO(logger_, "Threads initialized count={}", threadCount_);
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
        LOG_DEBUG(logger_, "Worker start tid={}", std::this_thread::get_id());
        for (;;) {
            while (localToken == dispatchToken_.load(std::memory_order_acquire) &&
                   !shutdown_.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (shutdown_.load(std::memory_order_acquire)) break;
            localToken = dispatchToken_.load(std::memory_order_acquire);
            processActiveRange();
        }
        LOG_DEBUG(logger_, "Worker exit tid={}", std::this_thread::get_id());
    }

    void processActiveRange() {
        for (;;) {
            std::size_t idx = nextChunk_.fetch_add(1, std::memory_order_relaxed);
            if (idx >= active_.totalChunks) break;
            std::size_t begin = idx * active_.chunkSize;
            std::size_t end   = std::min(begin + active_.chunkSize, active_.elementCount);
            if (settings_.logRangeTasks)
                LOG_TRACE(logger_, "ChunkStart tid={} idx={} b={} e={}",
                          std::this_thread::get_id(), idx, begin, end);
            (*active_.task)(begin, end, active_.frame, active_.dt);
            std::size_t rem = remaining_.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (settings_.logRangeTasks)
                LOG_TRACE(logger_, "ChunkDone tid={} idx={} rem={}",
                          std::this_thread::get_id(), idx, rem);
            if (rem == 0) break;
        }
    }

    /* ---- Main advance ---- */
    bool advance() {
        if (terminate_) return false;
        if (settings_.maxFrames >= 0 && frame_ >= settings_.maxFrames) return false;

        // Run the frame immediately
        doOneStep();
        // Advance target using correct duration type
        nextFrameTarget_ += std::chrono::duration_cast<Clock::duration>(dtMicro_);

        // Sleep/spin until target
        auto spinBudget = std::chrono::microseconds(settings_.spinMicros);
        for (;;) {
            auto now = Clock::now();
            if (now + spinBudget >= nextFrameTarget_) {
                while (Clock::now() < nextFrameTarget_)
                    std::this_thread::yield();
                break;
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }

        if (settings_.adaptive) {
            logDrift();
            auto behind = Clock::now() - nextFrameTarget_;
            if (behind.count() > 0) {
                int extra = int(std::chrono::duration<double>(behind).count() / dtMicro_.count());
                if (extra > settings_.maxCatchUp) extra = settings_.maxCatchUp;
                for (int i=0;i<extra;i++) {
                    if (settings_.maxFrames >= 0 && frame_ >= settings_.maxFrames) break;
                    doOneStep();
                }
            }
        } else {
            logDrift();
        }

        return !(settings_.maxFrames >= 0 && frame_ >= settings_.maxFrames);
    }

    void doOneStep() {
        PROF_SCOPE(profiler_, "Frame");
        for (auto& ph : phases_) {
            if (!ph.enabled) continue;
            if (settings_.logPhases)
                LOG_DEBUG(logger_, "PhaseBegin '{}' frame={}", ph.name, frame_);
            PROF_SCOPE(profiler_, "Phase:" + ph.name);

            for (auto& sub : ph.serialSubsystems)
                sub(frame_, dtMicro_);

            if (threadCount_ > 1 &&
                !ph.parallelRangeTasks.empty() &&
                ph.elementCount > 0) {
                std::size_t count = ph.elementCount;
                for (std::size_t tIdx=0; tIdx < ph.parallelRangeTasks.size(); ++tIdx) {
                    auto& rt = ph.parallelRangeTasks[tIdx];
                    std::size_t chunk = settings_.chunkSize ? settings_.chunkSize : 256;
                    std::size_t totalChunks = (count + chunk - 1)/chunk;
                    active_.task         = &rt;
                    active_.totalChunks  = totalChunks;
                    active_.elementCount = count;
                    active_.chunkSize    = chunk;
                    active_.frame        = frame_;
                    active_.dt           = dtMicro_;
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
                        rt(begin, end, frame_, dtMicro_);
                        if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1)
                            break;
                    }
                }
            } else {
                for (auto& rt : ph.parallelRangeTasks) {
                    PROF_SCOPE(profiler_, "RangeTask:" + ph.name + ":S");
                    rt(0, ph.elementCount, frame_, dtMicro_);
                }
            }

            for (auto& red : ph.reductions) {
                PROF_SCOPE(profiler_, "Reduction:" + ph.name);
                red(frame_, dtMicro_);
            }

            if (settings_.logPhases)
                LOG_DEBUG(logger_, "PhaseEnd   '{}' frame={}", ph.name, frame_);
        }
        ++frame_;
        if ((frame_ & 0x3FF) == 0)
            LOG_INFO(logger_, "Progress frame={}", frame_);
    }

    void logDrift() {
        if (settings_.driftLogInterval <= 0) return;
        if (frame_ % settings_.driftLogInterval) return;
        auto now = Clock::now();
        double simT  = static_cast<double>(frame_) * dtMicro_.count();
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
        dtMicro_   = Seconds{1.0 / settings_.hz};
        outerDt_   = dtMicro_ * subSteps_;
        outerDtChrono_ = std::chrono::duration_cast<Clock::duration>(outerDt_);
        startReal_ = Clock::now();
    }

    Settings               settings_{};
    std::vector<Phase>     phases_;
    std::int64_t           frame_      = 0;
    bool                   terminate_  = false;

    int                    subSteps_   = 1;
    Seconds                dtMicro_{1.0 / 500.0};
    Seconds                outerDt_{dtMicro_};
    Clock::duration        outerDtChrono_{};
    Clock::time_point      nextFrameTarget_{};
    Clock::time_point      startReal_{};
    Seconds                accumulator_{0};

    std::vector<std::thread> threads_;
    std::size_t              threadCount_ = 0;
    std::atomic<bool>        shutdown_{false};

    ActiveRange              active_{};
    std::atomic<std::size_t> nextChunk_{0};
    std::atomic<std::size_t> remaining_{0};
    std::atomic<std::uint64_t> dispatchToken_{0};

    std::uint64_t            deterministicHash_ = 0;
    double                   lastDriftMs_ = 0.0;

    Logger*   logger_   = nullptr;
    Profiler* profiler_ = nullptr;
};
