#include "simcore.hpp"
#include "logger.hpp"
#include "profiler.hpp"
#include <vector>
#include <cmath>
#include <iostream>
#include <cstring>
#include <sstream>
#include <iomanip>

static double parseDouble(const char* s, double def){ if(!s) return def; char* e=nullptr; double v=strtod(s,&e); return (e && *e==0)? v: def; }
static long   parseLong  (const char* s, long def){ if(!s) return def; char* e=nullptr; long v=strtol(s,&e,10); return (e && *e==0)? v: def; }
static std::size_t parseSize(const char* s, std::size_t def){
    if(!s) return def; char* e=nullptr; unsigned long long v=strtoull(s,&e,10); return (e && *e==0)? (std::size_t)v : def;
}

int main(int argc, char* argv[]) {
    SimCore::Settings cfg;
    cfg.hz = 1000.0;
    cfg.maxFrames = 3000;
    cfg.threads = std::max<std::size_t>(2, std::thread::hardware_concurrency());
    cfg.chunkSize = 128;
    cfg.maxCatchUp = 32;
    cfg.adaptiveThresholdFrames = 1.0; // count burst if >1 step
    cfg.logChunks = false;
    cfg.logRangeTasks = true;
    cfg.logPhases = true;
    cfg.driftLogInterval = 250;  // log drift every 250 frames
    bool stress = false;

    std::size_t elements = 5000;

    for (int i=1;i<argc;++i){
        if (std::strcmp(argv[i],"--stress")==0) stress = true;
        else if (std::strcmp(argv[i],"--hz")==0 && i+1<argc) cfg.hz = parseDouble(argv[++i], cfg.hz);
        else if (std::strcmp(argv[i],"--frames")==0 && i+1<argc) cfg.maxFrames = parseLong(argv[++i], cfg.maxFrames);
        else if (std::strcmp(argv[i],"--threads")==0 && i+1<argc) cfg.threads = parseSize(argv[++i], cfg.threads);
        else if (std::strcmp(argv[i],"--chunk")==0 && i+1<argc) cfg.chunkSize = parseSize(argv[++i], cfg.chunkSize);
        else if (std::strcmp(argv[i],"--maxCatchUp")==0 && i+1<argc) cfg.maxCatchUp = (int)parseLong(argv[++i], cfg.maxCatchUp);
        else if (std::strcmp(argv[i],"--thresholdFrames")==0 && i+1<argc) cfg.adaptiveThresholdFrames = parseDouble(argv[++i], cfg.adaptiveThresholdFrames);
        else if (std::strcmp(argv[i],"--elements")==0 && i+1<argc) elements = parseSize(argv[++i], elements);
        else if (std::strcmp(argv[i],"--adaptive")==0 && i+1<argc) cfg.adaptive = (std::atoi(argv[++i])!=0);
        else if (std::strcmp(argv[i],"--spinMicros")==0 && i+1<argc) cfg.spinMicros = (int)parseLong(argv[++i], cfg.spinMicros);
    }

    Logger logger;
#ifdef LOG_ENABLED
    logger.setLevel(Logger::Level::Info);
    logger.addSink(std::make_shared<Logger::StdoutSink>());
#endif

    Profiler profiler;
    SimCore sim(cfg);
    sim.setLogger(&logger);
    sim.setProfiler(&profiler);

    auto input   = sim.addPhase("Input");
    auto physics = sim.addPhase("Physics");

    std::vector<double> pos(elements,0.0), vel(elements,10.0),
                        thr(elements,0.5), force(elements,0.0);
    sim.setPhaseElementCount(physics, elements);

    // Serial input phase (control/throttle modulation + optional stalls)
    sim.addSerialSubsystem(input, [&](std::int64_t f, SimCore::Seconds dt){
        double t = f * dt.count();
        for (std::size_t i=0;i<elements;++i)
            thr[i] = 0.5 + 0.05 * std::sin(t + i*0.0005);

        if (stress) {
            // Inject periodic stalls every 750 frames
            if (f>0 && f % 750 == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                LOG_INFO(&logger, "[STALL] 5ms frame={}", f);
            }
        }
    });

    // Force computation (parallel)
    sim.addParallelRangeTask(physics, [&](std::size_t b, std::size_t e,
                                          std::int64_t, SimCore::Seconds){
        for (std::size_t i=b;i<e;++i)
            force[i] = thr[i] * 1000.0;
    });

    // Integrate (parallel)
    sim.addParallelRangeTask(physics, [&](std::size_t b, std::size_t e,
                                          std::int64_t, SimCore::Seconds dt){
        double dts = dt.count();
        for (std::size_t i=b;i<e;++i){
            vel[i] += (force[i] / 1200.0) * dts;
            pos[i] += vel[i] * dts;
        }
    });

    // Deterministic reduction with hash / progress
    sim.addReductionTask(physics, [&](std::int64_t f, SimCore::Seconds dt){
        if (f % 1000 == 0) {
            std::uint64_t h = 1469598103934665603ull;
            for (double v : vel) {
                std::uint64_t bits;
                std::memcpy(&bits, &v, sizeof(v));
                h ^= bits;
                h *= 1099511628211ull;
            }
            sim.setDeterministicHash(h);
            double sum=0.0; for (double v : vel) sum += v;
            double avg = sum / vel.size();
            std::ostringstream hex;
            hex<<"0x"<<std::hex<<std::setw(16)<<std::setfill('0')<<h;
            LOG_INFO(&logger, "[REDUCE] frame={} avgVel={} hash={}", f, avg, hex.str());
        }
    });

    sim.run();

    std::cout << "Final frame=" << sim.frame()
              << " pos0=" << pos[0]
              << " vel0=" << vel[0]
              << " hash=0x" << std::hex << sim.deterministicHash() << std::dec
              << "\n";

    if (cfg.adaptive) {
        std::cout << "AdaptiveStats bursts=" << sim.bursts()
                  << " extraSteps=" << sim.extraSteps()
                  << " recoveredMs=" << std::fixed << std::setprecision(2) << sim.recoveredMs()
                  << "\n";
    }
}
