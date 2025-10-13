#include <simcore/SimCore.hpp>
#include <simcore/logger.hpp>
#include <simcore/metrics.hpp>
#include <simcore/arena.hpp>
#include <simcore/car_soa.hpp>

#include <vector>
#include <cmath>
#include <iostream>
#include <cstring>
#include <iomanip>
#include <string>
#include <cstdint>
#include <fstream>
#include <iterator>

/* tiny helpers --------------------------------------------------- */
static void printHelp(const char* argv0)
{
    std::cout << "Usage: " << argv0 << " [options]\n\n"
              << "Options:\n"
              << "  --threads <n>              Number of worker threads (default: hardware concurrency).\n"
              << "  --pin                      Pin worker threads to cores.\n"
              << "  --metrics-json             Emit a cumulative metrics snapshot at exit (p50/p95/p99 span the full run).\n"
              << "  --metrics-json-interval    Emit metrics JSON and reset rolling histograms and resettable counters after each emission.\n"
              << "  --snapshot-out <file>      Write the final simulation snapshot to <file>.\n"
              << "  --snapshot-in <file>       Load the simulation snapshot from <file> before running.\n"
              << "  --help, -h                 Show this help message.\n";
}

static std::size_t parseSize(const char* s, std::size_t def)
{
    if (!s) return def;
    char* e = nullptr;
    unsigned long long v = std::strtoull(s, &e, 10);
    return (e && *e == 0) ? static_cast<std::size_t>(v) : def;
}

int main(int argc, char** argv)
{
    /* ------------ config via CLI ---------------- */
    SimCore::Settings cfg;
    cfg.hz        = 1000.0;
    cfg.maxFrames = 3000;
    cfg.chunkSize = 128;

    bool metricsJson = false;
    bool metricsJsonInterval = false;
    std::string snapshotOutPath;
    std::string snapshotInPath;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
        {
            printHelp(argv[0]);
            return 0;
        }
        else if (std::strcmp(argv[i], "--elements") == 0 && i + 1 < argc)
            ; // handled later
        else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            cfg.threads = parseSize(argv[++i], cfg.threads);
        else if (std::strcmp(argv[i], "--pin") == 0)
            cfg.pinThreads = true;
        else if (std::strcmp(argv[i], "--metrics-json") == 0)
            metricsJson = true;
        else if (std::strcmp(argv[i], "--metrics-json-interval") == 0)
        {
            metricsJson = true;
            metricsJsonInterval = true;
        }
        else if (std::strcmp(argv[i], "--snapshot-out") == 0 && i + 1 < argc)
            snapshotOutPath = argv[++i];
        else if (std::strcmp(argv[i], "--snapshot-in") == 0 && i + 1 < argc)
            snapshotInPath = argv[++i];
        else if (std::strcmp(argv[i], "--snapshot-out") == 0 || std::strcmp(argv[i], "--snapshot-in") == 0)
        {
            std::cerr << "Missing path for " << argv[i] << "\n";
            return 1;
        }
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
    Logger *loggerPtr = &logger;

    metrics::Metrics metricsRegistry;

    SimCore sim(cfg);
    sim.setLogger(loggerPtr);
    if (metricsJson)
        sim.setMetrics(&metricsRegistry);

    auto input   = sim.addPhase("Input");
    auto physics = sim.addPhase("Physics");
    sim.setPhaseElementCount(physics, N);

    /* -------------- subsystems ------------------ */
    sim.addSerialSubsystem(input, [&](int64_t f, SimCore::Seconds dt){
        double t = static_cast<double>(f) * dt.count();
        for (std::size_t i = 0; i < N; ++i)
            thr[i] = 0.5 + 0.05 * std::sin(t + static_cast<double>(i) * 0.0005);
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
            double avg = sum / static_cast<double>(cars.size());
            LOG_INFO(loggerPtr, "[REDUCE] frame={} avgVel={:.4f}", f, avg);
        }
    });

    if (!snapshotInPath.empty())
    {
        std::ifstream in(snapshotInPath, std::ios::binary);
        if (!in)
        {
            std::cerr << "Failed to open snapshot input file: " << snapshotInPath << "\n";
            return 1;
        }
        std::vector<std::uint8_t> snapshotData((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (snapshotData.empty())
        {
            std::cerr << "Snapshot input file is empty: " << snapshotInPath << "\n";
            return 1;
        }
        sim.loadFrame(snapshotData);
    }

    sim.run();

    if (!snapshotOutPath.empty())
    {
        auto data = sim.saveFrame();
        std::ofstream out(snapshotOutPath, std::ios::binary);
        if (!out)
        {
            std::cerr << "Failed to open snapshot output file: " << snapshotOutPath << "\n";
            return 1;
        }
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out)
        {
            std::cerr << "Failed to write snapshot output file: " << snapshotOutPath << "\n";
            return 1;
        }
    }

    if (metricsJson)
        std::cout << metricsRegistry.snapshot(metricsJsonInterval) << "\n";
    else
        std::cout << "Final pos0=" << cars.pos[0] << "\n";
    return 0;
}
