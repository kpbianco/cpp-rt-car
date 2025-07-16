#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

class SimCore {
public:
    using Clock     = std::chrono::steady_clock;
    using Seconds   = std::chrono::duration<double>;
    using Subsystem = std::function<void(std::int64_t, Seconds)>;

    struct Settings {
        double        hz         = 400.0;   // physics‑solver rate
        std::int64_t  maxFrames  = -1;      // −1 → endless
        bool          adaptive   = true;    // catch‑up ticks when behind
        int           maxCatchUp = 4;       // safety cap
    };

    SimCore()                       { applySettings(Settings{}); }
    explicit SimCore(const Settings& s) { applySettings(s); }

    void addSubsystem(Subsystem f) { subsystems_.emplace_back(std::move(f)); }

    void setHz(double hz)          { Settings tmp = settings_; tmp.hz = hz; applySettings(tmp); }
    double hz()        const       { return hzAtom_.load(std::memory_order_relaxed); }
    double dtSeconds() const       { return dtMicro_.count(); }
    std::int64_t currentFrame() const { return frame_; }
    void requestExit()             { terminate_ = true; }

    template<bool Multithread = false>
    void run()
    {
        auto nextOuter = Clock::now();

        const auto minSleep = std::chrono::milliseconds(1);   // OS‑level resolution

        while (outerStep<Multithread>()) {
            nextOuter += outerDtChrono_;

            if (outerDtChrono_ > minSleep)
                std::this_thread::sleep_until(nextOuter);
            else
                while (Clock::now() < nextOuter) std::this_thread::yield();

            if (settings_.adaptive) {
                auto lag = Clock::now() - nextOuter;
                int  lagOuters = static_cast<int>(
                        std::chrono::duration<double>(lag).count() / outerDt_.count());
                lagOuters = std::clamp(lagOuters, 0, settings_.maxCatchUp);
                for (int i = 0; i < lagOuters; ++i) outerStep<Multithread>();
                nextOuter = Clock::now();
            }
        }
    }

private:
    void applySettings(const Settings& s)
    {
        settings_ = s;
        hzAtom_.store(s.hz, std::memory_order_relaxed);

        // group micro‑steps so outer loop ≥1 kHz
        subSteps_  = (s.hz > 1000.0) ? static_cast<int>(std::ceil(s.hz / 1000.0)) : 1;
        dtMicro_   = Seconds{1.0 / s.hz};
        outerDt_   = dtMicro_ * subSteps_;
        outerDtChrono_ = std::chrono::duration_cast<Clock::duration>(outerDt_);
    }

    template<bool Multithread>
    bool outerStep()
    {
        if (terminate_ || (settings_.maxFrames >= 0 && frame_ >= settings_.maxFrames))
            return false;

        for (int i = 0; i < subSteps_; ++i)
        {
            if constexpr (Multithread) {
                std::vector<std::thread> pool;
                pool.reserve(subsystems_.size());
                for (auto& f : subsystems_) pool.emplace_back(f, frame_, dtMicro_);
                for (auto& t : pool) t.join();
            } else {
                for (auto& f : subsystems_) f(frame_, dtMicro_);
            }
            ++frame_;
        }
        return true;
    }

    Settings                       settings_{};
    std::atomic<double>            hzAtom_{settings_.hz};
    int                             subSteps_   = 1;
    Seconds                         dtMicro_{1.0 / settings_.hz};
    Seconds                         outerDt_{dtMicro_};
    Clock::duration                 outerDtChrono_{};
    std::vector<Subsystem>          subsystems_;
    std::int64_t                    frame_      = 0;
    bool                            terminate_  = false;
};
