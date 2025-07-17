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

    //default constructor, no params
    SimCore() { 
        applySettings(Settings{}); 
    }

    //default constructor with settings struct passed
    explicit SimCore(const Settings& s) { 
        applySettings(s); 
    }

    //"fifo buffer" that takes in a lambda or function and executes in order of appearance
    void addSubsystem(Subsystem f) { 
        subsystems_.emplace_back(std::move(f)); 
    }

    //set atomic clock to new value and make sure delta is updated
    void setHz(double hz) noexcept {
        settings_.hz = hz;
        hzAtom_.store(hz, std::memory_order_relaxed);
        recalcTimingFromHz();   
    }

    //pull atomic clock value
    double hz() const       { 
        return hzAtom_.load(std::memory_order_relaxed); 
    }

    //pull calculated delta
    double dtSeconds() const       { 
        return dtMicro_.count(); 
    }

    //grab current frame num
    std::int64_t currentFrame() const { 
        return frame_; 
    }

    //set exit flag which updates state automatically
    void requestExit()             { 
        terminate_ = true; 
    }

    template<bool Multithread = false>
    void run()
    {
        auto nextFrame = Clock::now(); //OS iterable sleep increments

        // OS cannot sleep longer, so if we take longer we call sleep until
        const auto minSleep = std::chrono::milliseconds(1);   

        //continues until max steps or terminate
        while (processFrameStep<Multithread>()) {

            //next frame deadline (cumulative)
            nextFrame += outerDtChrono_;
            
            //let OS handle sleep
            if (outerDtChrono_ > minSleep)
                std::this_thread::sleep_until(nextFrame);
            else //busy wait with our method
                while (Clock::now() < nextFrame) std::this_thread::yield();

            //if we catchup with lag, determine the time behind and correct
            if (settings_.adaptive) {
                auto lag = Clock::now() - nextFrame; // how much overslept
                // ms of oversleep
                int  lagOuters = static_cast<int>(
                        std::chrono::duration<double>(lag).count() / frameDt_.count());

                //max catchups allocated/allowed
                lagOuters = std::clamp(lagOuters, 0, settings_.maxCatchUp);
                //actively catch back
                for (int i = 0; i < lagOuters; ++i) processFrameStep<Multithread>();
                //reset reference clock to current clock to prevent cumulative drift
                nextFrame = Clock::now();
            }
        }
    }

private:
    //because hz has more implications, it cannot use normal setter
    void applySettings(const Settings& s) {
        settings_ = s;
        hzAtom_.store(s.hz, std::memory_order_relaxed);
        recalcTimingFromHz();
    }

    //compute duration of one micro step (1/hz)and decide how many to group for each frame
    void recalcTimingFromHz() noexcept {
        const double hz = settings_.hz;

        // if 1/hz >= 1m do 1 normal step, if smaller group the steps
        // for example 1000 hz would have 1ms step per loop
        // 5000 hz would have 5 steps at 200us steps per loop
        subSteps_  = (hz > 1000.0) ? static_cast<int>(std::ceil(hz / 1000.0)) : 1;

        dtMicro_   = Seconds{1.0 / hz}; //duration in second of seconds
        frameDt_   = dtMicro_ * subSteps_; //total time of one frame
        //convert total frame time into native object
        outerDtChrono_ = std::chrono::duration_cast<Clock::duration>(frameDt_);
    }

    template<bool Multithread> //runs each subsystem in its own thread
    bool processFrameStep()
    {
        //if terminate is set or maxframes hit abort
        if (terminate_ || (settings_.maxFrames >= 0 && frame_ >= settings_.maxFrames))
            return false;

        //iterate through substeps to comprise one frame
        for (int i = 0; i < subSteps_; ++i)
        {
            //spawn a thread per subsystem and calls their associated function
            // waits for thread to join across all threads
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
    Seconds                         frameDt_{dtMicro_};
    Clock::duration                 outerDtChrono_{};
    std::vector<Subsystem>          subsystems_;
    std::int64_t                    frame_      = 0;
    bool                            terminate_  = false;
};
