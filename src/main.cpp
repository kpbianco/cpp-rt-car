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
#include <fstream>
#include <filesystem>
#include <iterator>
#include <type_traits>

#include <rt/snapshot.hpp>

/* tiny helpers --------------------------------------------------- */
static void printHelp(const char* argv0)
{
    std::cout << "Usage: " << argv0 << " [options]\n\n"
              << "Options:\n"
              << "  --threads <n>              Number of worker threads (default: hardware concurrency).\n"
              << "  --pin                      Pin worker threads to cores.\n"
              << "  --metrics-json             Emit a cumulative metrics snapshot at exit (p50/p95/p99 span the full run).\n"
              << "  --metrics-json-interval    Emit metrics JSON and reset rolling histograms and resettable counters after each emission.\n"
              << "  --snapshot-in <path>       Load a binary snapshot before running.\n"
              << "  --snapshot-out <path>      Save the final binary snapshot.\n"
              << "  --fma                      Enable fused multiply-add operations.\n"
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
    cfg.rateGovernorEnable = false;
    cfg.predictiveEnable   = false;
    cfg.budgetMonitor      = false;
    cfg.autoTuneChunks     = false;
    cfg.spinMicros         = 0;

    bool metricsJson = false;
    bool metricsJsonInterval = false;
    std::filesystem::path snapshotIn;
    std::filesystem::path snapshotOut;

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
        else if (std::strcmp(argv[i], "--snapshot-in") == 0 && i + 1 < argc)
            snapshotIn = argv[++i];
        else if (std::strcmp(argv[i], "--snapshot-out") == 0 && i + 1 < argc)
            snapshotOut = argv[++i];
        else if (std::strcmp(argv[i], "--fma") == 0)
            cfg.useFMA = true;
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

    auto writeVector = [](rt::SnapshotWriter &w, const auto &vec) {
        using VecType = std::remove_reference_t<decltype(vec)>;
        using Value = typename VecType::value_type;
        std::uint64_t n = static_cast<std::uint64_t>(vec.size());
        w.write(n);
        if (n == 0) return;
        static_assert(std::is_trivially_copyable_v<Value>, "snapshot vector requires trivially copyable type");
        const auto *ptr = reinterpret_cast<const std::uint8_t *>(vec.data());
        w.data.insert(w.data.end(), ptr, ptr + n * sizeof(Value));
    };

    auto readVector = [](rt::SnapshotReader &r, auto &vec) {
        using VecType = std::remove_reference_t<decltype(vec)>;
        using Value = typename VecType::value_type;
        std::uint64_t n = 0;
        r.read(n);
        vec.resize(static_cast<std::size_t>(n));
        if (n == 0) return;
        static_assert(std::is_trivially_copyable_v<Value>, "snapshot vector requires trivially copyable type");
        std::memcpy(vec.data(), r.data.data() + r.offset, n * sizeof(Value));
        r.offset += n * sizeof(Value);
    };

    constexpr std::uint32_t kSnapshotMagic = 0x52544657u; // 'RTFW'

    if (!snapshotIn.empty())
    {
        std::ifstream in(snapshotIn, std::ios::binary);
        if (!in)
        {
            std::cerr << "Failed to open snapshot input: " << snapshotIn << "\n";
            return 1;
        }
        std::vector<std::uint8_t> buffer((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (buffer.size() < sizeof(kSnapshotMagic))
        {
            std::cerr << "Snapshot input truncated: " << snapshotIn << "\n";
            return 1;
        }
        rt::SnapshotReader reader(buffer);
        std::uint32_t magic = 0;
        reader.read(magic);
        if (magic != kSnapshotMagic)
        {
            std::cerr << "Snapshot magic mismatch for " << snapshotIn << "\n";
            return 1;
        }

        std::vector<std::uint8_t> simState;
        readVector(reader, simState);
        sim.loadFrame(simState);
        readVector(reader, thr);
        readVector(reader, force);
        readVector(reader, cars.pos);
        readVector(reader, cars.vel);
        readVector(reader, cars.sparse);
        readVector(reader, cars.dense);
        reader.read(cars.defaultPos);
        reader.read(cars.defaultVel);
    }

    sim.run();

    auto buildSnapshot = [&]() {
        rt::SnapshotWriter writer;
        writer.write(kSnapshotMagic);
        writeVector(writer, sim.saveFrame());
        writeVector(writer, thr);
        writeVector(writer, force);
        writeVector(writer, cars.pos);
        writeVector(writer, cars.vel);
        writeVector(writer, cars.sparse);
        writeVector(writer, cars.dense);
        writer.write(cars.defaultPos);
        writer.write(cars.defaultVel);
        return writer.data;
    };

    auto snapshotData = buildSnapshot();
    std::uint64_t hash = rt::hash64(snapshotData);

    if (!snapshotOut.empty())
    {
        std::ofstream out(snapshotOut, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            std::cerr << "Failed to open snapshot output: " << snapshotOut << "\n";
            return 1;
        }
        out.write(reinterpret_cast<const char *>(snapshotData.data()), static_cast<std::streamsize>(snapshotData.size()));
        if (!out)
        {
            std::cerr << "Failed to write snapshot output: " << snapshotOut << "\n";
            return 1;
        }
    }

    if (metricsJson)
        std::cout << metricsRegistry.snapshot(metricsJsonInterval) << "\n";
    else
        std::cout << "Final pos0=" << cars.pos[0] << "\n";
    std::cout << "Final hash=" << std::hex << std::setfill('0') << std::setw(16) << hash << std::dec << "\n";
    return 0;
}
