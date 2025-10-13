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

    auto canonicalizeSimState = [](std::vector<std::uint8_t> &snapshot) {
        // SimCore snapshots encode scheduler bookkeeping such as chosen chunk sizes
        // and per-phase totals. Those fields depend on the active worker-count, so
        // erase them to keep the serialized state identical across thread topologies.
        constexpr std::size_t magicSize = sizeof(kSnapshotMagic);
        if (snapshot.size() <= magicSize + sizeof(std::uint64_t)) {
            return;
        }

        auto readU64 = [](const std::uint8_t *ptr) {
            std::uint64_t v = 0;
            std::memcpy(&v, ptr, sizeof(v));
            return v;
        };
        auto writeU64 = [](std::uint8_t *ptr, std::uint64_t value) {
            std::memcpy(ptr, &value, sizeof(value));
        };
        auto writeF64 = [](std::uint8_t *ptr, double value) {
            std::memcpy(ptr, &value, sizeof(value));
        };

        std::size_t offset = magicSize;
        std::uint64_t simLen = readU64(snapshot.data() + offset);
        offset += sizeof(std::uint64_t);
        if (snapshot.size() < offset + simLen) {
            return;
        }

        std::uint8_t *simBytes = snapshot.data() + offset;
        std::size_t simSize = static_cast<std::size_t>(simLen);
        std::size_t cursor = 0;

        auto advance = [&](std::size_t bytes) {
            if (cursor > simSize || simSize - cursor < bytes) {
                cursor = simSize;
                return false;
            }
            cursor += bytes;
            return true;
        };
        auto readU64Sim = [&](std::uint64_t &out) {
            if (!advance(sizeof(std::uint64_t))) {
                out = 0;
                return false;
            }
            std::memcpy(&out, simBytes + cursor - sizeof(std::uint64_t), sizeof(std::uint64_t));
            return true;
        };
        auto zeroU64At = [&](std::size_t pos) {
            if (pos + sizeof(std::uint64_t) <= simSize) {
                writeU64(simBytes + pos, 0);
            }
        };
        auto zeroF64At = [&](std::size_t pos) {
            if (pos + sizeof(double) <= simSize) {
                writeF64(simBytes + pos, 0.0);
            }
        };

        std::uint64_t ignored = 0;
        for (int i = 0; i < 4; ++i) {
            readU64Sim(ignored);
        }

        std::uint64_t phaseCount = 0;
        if (!readU64Sim(phaseCount)) {
            return;
        }

        for (std::uint64_t i = 0; i < phaseCount; ++i) {
            std::uint64_t sampleLen = 0;
            if (!readU64Sim(sampleLen)) {
                return;
            }
            if (!advance(static_cast<std::size_t>(sampleLen) * sizeof(std::uint64_t))) {
                return;
            }

            if (!readU64Sim(ignored)) { // element count
                return;
            }
            if (!advance(sizeof(std::uint8_t))) { // enabled flag
                return;
            }

            std::size_t chosenPos = cursor;
            if (!readU64Sim(ignored)) {
                return;
            }
            zeroU64At(chosenPos);

            std::size_t totalPos = cursor;
            if (!readU64Sim(ignored)) {
                return;
            }
            zeroU64At(totalPos);

            std::size_t skewPos = cursor;
            if (!readU64Sim(ignored)) {
                return;
            }
            zeroU64At(skewPos);

            std::size_t cusumPos = cursor;
            if (!advance(sizeof(double))) {
                return;
            }
            zeroF64At(cusumPos);

            if (cursor < simSize) {
                simBytes[cursor] = 0; // pinned flag
            }
            if (!advance(sizeof(std::uint8_t))) {
                return;
            }
        }
    };

    auto buildSnapshot = [&]() {
        rt::SnapshotWriter writer;
        writer.write(kSnapshotMagic);
        auto simState = sim.saveFrame();
        writer.writeVector(simState);
        writeVector(writer, thr);
        writeVector(writer, force);
        writeVector(writer, cars.pos);
        writeVector(writer, cars.vel);
        writeVector(writer, cars.sparse);
        writeVector(writer, cars.dense);
        writer.write(cars.defaultPos);
        writer.write(cars.defaultVel);

        canonicalizeSimState(writer.data);
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
