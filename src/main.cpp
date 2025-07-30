#include "simcore.hpp"
#include "logger.hpp"
#include "arena.hpp"
#include "car_soa.hpp"

#include <vector>
#include <cmath>
#include <iostream>
#include <cstring>
#include <iomanip>

/* tiny helpers --------------------------------------------------- */
static std::size_t parseSize(const char* s, std::size_t def)
{
    if (!s) return def;
    char* e = nullptr;
    unsigned long long v = std::strtoull(s, &e, 10);
    return (e && *e == 0) ? static_cast<std::size_t>(v) : def;
}
static double parseDouble(const char* s, double def)
{
    if (!s) return def;
    char* e = nullptr;
    double v = std::strtod(s, &e);
    return (e && *e == 0) ? v : def;
}

int main(int argc, char** argv)
{
    /* ------------ config via CLI ---------------- */
    SimCore::Settings cfg;
    cfg.hz        = 1000.0;
    cfg.maxFrames = 3000;
    cfg.chunkSize = 128;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--elements") == 0 && i + 1 < argc)
            ; // handled later
        else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            cfg.threads = parseSize(argv[++i], cfg.threads);
        else if (std::strcmp(argv[i], "--pin") == 0)
            cfg.pinThreads = true;
    }

    /* ------------ arena + arrays ---------------- */
    constexpr std::size_t N = 5'000;
    FrameArena arena;
    CarSoA cars(N);                      // SoA backing vectors

    std::vector<double> thr(N, 0.5);     // scalar arrays stay AoS for now
    std::vector<double> force(N, 0.0);

    Logger logger;
    logger.setLevel(Logger::Level::Info);
    logger.addSink(std::make_shared<Logger::StdoutSink>());

    SimCore sim(cfg);
    sim.setLogger(&logger);

    auto input   = sim.addPhase("Input");
    auto physics = sim.addPhase("Physics");
    sim.setPhaseElementCount(physics, N);

    /* -------------- subsystems ------------------ */
    sim.addSerialSubsystem(input, [&](int64_t f, SimCore::Seconds dt){
        double t = f * dt.count();
        for (std::size_t i = 0; i < N; ++i)
            thr[i] = 0.5 + 0.05 * std::sin(t + i * 0.0005);
    });

    sim.addParallelRangeTask(physics, [&](std::size_t b, std::size_t e,
                                          int64_t, SimCore::Seconds) {
        for (std::size_t i = b; i < e; ++i)
            force[i] = thr[i] * 1000.0;
    });

    sim.addParallelRangeTask(physics, [&](std::size_t b, std::size_t e,
                                          int64_t, SimCore::Seconds dt) {
        double dts = dt.count();
        for (std::size_t i = b; i < e; ++i) {
            cars.vel[i] += (force[i] / 1200.0) * dts;
            cars.pos[i] += cars.vel[i] * dts;
        }
    });

    /* deterministic reduction */
    sim.addReductionTask(physics, [&](int64_t f, SimCore::Seconds){
        if (f % 1000 == 0) {
            double sum = 0.0;
            for (double v : cars.vel) sum += v;
            double avg = sum / cars.size();
            LOG_INFO(&logger, "[REDUCE] frame={} avgVel={:.4f}", f, avg);
        }
    });

    sim.run();

    std::cout << "Final pos0=" << cars.pos[0] << "\n";
    return 0;
}
