#include <simcore/SimCore.hpp>
#include <simcore/logger.hpp>
#include <simcore/metrics.hpp>
#include <simcore/arena.hpp>
#include <simcore/car_soa.hpp>
#include <rt/snapshot.hpp>

#include <algorithm>
#include <array>
#include <vector>
#include <cmath>
#include <iostream>
#include <cstring>
#include <iomanip>
#include <string>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <type_traits>

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

namespace
{

static constexpr std::array<char, 8> kSnapshotMagic{'R', 'T', 'F', 'W', 'S', 'N', 'A', 'P'};
static constexpr std::uint32_t kSnapshotVersion = 1u;

template <typename T>
void writeScalar(std::ofstream &out, T value)
{
    static_assert(std::is_trivially_copyable_v<T>, "writeScalar requires trivially copyable types");
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    if (!out)
        throw std::runtime_error("Failed to write scalar to snapshot");
}

template <typename T>
void writeVector(std::ofstream &out, const std::vector<T> &values)
{
    static_assert(std::is_trivially_copyable_v<T>, "writeVector requires trivially copyable types");
    std::uint64_t size = static_cast<std::uint64_t>(values.size());
    writeScalar(out, size);
    if (size == 0)
        return;
    out.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(sizeof(T) * values.size()));
    if (!out)
        throw std::runtime_error("Failed to write vector payload to snapshot");
}

template <typename T>
void readScalar(std::ifstream &in, T &value)
{
    static_assert(std::is_trivially_copyable_v<T>, "readScalar requires trivially copyable types");
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    if (!in)
        throw std::runtime_error("Failed to read scalar from snapshot");
}

template <typename T>
std::vector<T> readVector(std::ifstream &in)
{
    static_assert(std::is_trivially_copyable_v<T>, "readVector requires trivially copyable types");
    std::uint64_t size = 0;
    readScalar(in, size);
    if (size == 0)
        return {};
    std::vector<T> values(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(values.data()), static_cast<std::streamsize>(sizeof(T) * values.size()));
    if (!in)
        throw std::runtime_error("Failed to read vector payload from snapshot");
    return values;
}

struct DemoSnapshot
{
    std::vector<std::uint8_t> simState;
    std::vector<double> throttle;
    std::vector<double> force;
    std::vector<double> pos;
    std::vector<double> vel;
    std::vector<std::uint64_t> sparse;
    std::vector<std::uint64_t> dense;
};

std::vector<std::uint8_t> canonicalizeSimState(const std::vector<std::uint8_t> &raw)
{
    if (raw.empty())
        return raw;

    rt::SnapshotReader reader(raw);
    rt::SnapshotWriter writer;

    auto copyScalar = [&](auto value) {
        writer.write(value);
    };

    std::uint64_t frame = 0;
    reader.read(frame);
    copyScalar(frame);

    std::uint64_t rngSeed = 0;
    reader.read(rngSeed);
    copyScalar(rngSeed);

    std::uint64_t subSteps = 0;
    reader.read(subSteps);
    copyScalar(subSteps);

    std::uint64_t preSteps = 0;
    reader.read(preSteps);
    copyScalar(preSteps);

    std::uint64_t phaseCount = 0;
    reader.read(phaseCount);
    copyScalar(phaseCount);

    for (std::uint64_t i = 0; i < phaseCount; ++i)
    {
        std::vector<std::size_t> samples;
        reader.readVector(samples);
        std::vector<std::size_t> emptySamples;
        writer.writeVector(emptySamples);

        std::uint64_t elemCount = 0;
        reader.read(elemCount);
        copyScalar(elemCount);

        std::uint8_t enabled = 0;
        reader.read(enabled);
        copyScalar(enabled);

        std::uint64_t chunk = 0;
        reader.read(chunk);
        (void)chunk;
        std::uint64_t totalChunks = 0;
        reader.read(totalChunks);
        (void)totalChunks;
        std::uint64_t chunkSkew = 0;
        reader.read(chunkSkew);
        (void)chunkSkew;
        double chunkCusum = 0.0;
        reader.read(chunkCusum);
        (void)chunkCusum;
        std::uint8_t pinned = 0;
        reader.read(pinned);
        (void)pinned;

        std::uint64_t zero64 = 0;
        double zeroDouble = 0.0;
        std::uint8_t zero8 = 0;
        copyScalar(zero64);
        copyScalar(zero64);
        copyScalar(zero64);
        writer.write(zeroDouble);
        copyScalar(zero8);
    }

    std::vector<double> costWindow;
    reader.readVector(costWindow);
    std::vector<double> zeroCost(costWindow.size(), 0.0);
    writer.writeVector(zeroCost);

    std::uint64_t costHead = 0;
    reader.read(costHead);
    std::uint64_t zero64 = 0;
    copyScalar(zero64);

    std::uint64_t costCount = 0;
    reader.read(costCount);
    copyScalar(zero64);

    double costSum = 0.0;
    reader.read(costSum);
    double zeroDouble = 0.0;
    writer.write(zeroDouble);

    std::uint8_t primed = 0;
    reader.read(primed);
    std::uint8_t zero8 = 0;
    copyScalar(zero8);

    std::uint64_t degradeRung = 0;
    reader.read(degradeRung);
    copyScalar(degradeRung);

    std::uint64_t bursts = 0;
    reader.read(bursts);
    copyScalar(bursts);

    std::uint64_t extraSteps = 0;
    reader.read(extraSteps);
    copyScalar(extraSteps);

    double recoveredMs = 0.0;
    reader.read(recoveredMs);
    writer.write(recoveredMs);

    double precoveredMs = 0.0;
    reader.read(precoveredMs);
    writer.write(precoveredMs);

    return writer.data;
}

DemoSnapshot loadSnapshotFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Failed to open snapshot input file: " + path);

    std::array<char, kSnapshotMagic.size()> magic{};
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!in)
        throw std::runtime_error("Snapshot header truncated: " + path);
    if (magic != kSnapshotMagic)
        throw std::runtime_error("Snapshot magic mismatch: " + path);

    std::uint32_t version = 0;
    readScalar(in, version);
    if (version != kSnapshotVersion)
        throw std::runtime_error("Unsupported snapshot version: " + std::to_string(version));

    DemoSnapshot snapshot;
    snapshot.simState = readVector<std::uint8_t>(in);
    snapshot.throttle = readVector<double>(in);
    snapshot.force = readVector<double>(in);
    snapshot.pos = readVector<double>(in);
    snapshot.vel = readVector<double>(in);
    snapshot.sparse = readVector<std::uint64_t>(in);
    snapshot.dense = readVector<std::uint64_t>(in);

    return snapshot;
}

void storeSnapshotFile(const std::string &path,
                       const DemoSnapshot &snapshot)
{
    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw std::runtime_error("Failed to open snapshot output file: " + path);

    out.write(kSnapshotMagic.data(), static_cast<std::streamsize>(kSnapshotMagic.size()));
    if (!out)
        throw std::runtime_error("Failed to write snapshot magic: " + path);

    writeScalar(out, kSnapshotVersion);
    writeVector(out, snapshot.simState);
    writeVector(out, snapshot.throttle);
    writeVector(out, snapshot.force);
    writeVector(out, snapshot.pos);
    writeVector(out, snapshot.vel);
    writeVector(out, snapshot.sparse);
    writeVector(out, snapshot.dense);
}

} // namespace

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
    cfg.predictiveEnable = false;
    cfg.autoTuneChunks = false;
    cfg.budgetMonitor = false;
    cfg.rateGovernorEnable = false;
    cfg.logPhases = false;
    cfg.logRangeTasks = false;
    cfg.logChunks = false;

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
    logger.setLevel(Logger::Level::Warn);
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
        try
        {
            DemoSnapshot snapshot = loadSnapshotFile(snapshotInPath);
            if (!snapshot.simState.empty())
                sim.loadFrame(snapshot.simState);

            if (!snapshot.throttle.empty())
            {
                if (snapshot.throttle.size() != thr.size())
                    throw std::runtime_error("Snapshot throttle vector size mismatch");
                std::copy(snapshot.throttle.begin(), snapshot.throttle.end(), thr.begin());
            }
            if (!snapshot.force.empty())
            {
                if (snapshot.force.size() != force.size())
                    throw std::runtime_error("Snapshot force vector size mismatch");
                std::copy(snapshot.force.begin(), snapshot.force.end(), force.begin());
            }
            if (!snapshot.pos.empty())
            {
                if (snapshot.pos.size() != cars.pos.size())
                    throw std::runtime_error("Snapshot position vector size mismatch");
                std::copy(snapshot.pos.begin(), snapshot.pos.end(), cars.pos.begin());
            }
            if (!snapshot.vel.empty())
            {
                if (snapshot.vel.size() != cars.vel.size())
                    throw std::runtime_error("Snapshot velocity vector size mismatch");
                std::copy(snapshot.vel.begin(), snapshot.vel.end(), cars.vel.begin());
            }
            if (!snapshot.sparse.empty())
            {
                if (snapshot.sparse.size() != cars.sparse.size())
                    throw std::runtime_error("Snapshot sparse index size mismatch");
                for (std::size_t i = 0; i < snapshot.sparse.size(); ++i)
                    cars.sparse[i] = static_cast<std::size_t>(snapshot.sparse[i]);
            }
            if (!snapshot.dense.empty())
            {
                if (snapshot.dense.size() != cars.dense.size())
                    throw std::runtime_error("Snapshot dense index size mismatch");
                for (std::size_t i = 0; i < snapshot.dense.size(); ++i)
                    cars.dense[i] = static_cast<std::size_t>(snapshot.dense[i]);
            }
        }
        catch (const std::exception &ex)
        {
            std::cerr << "Snapshot load failed: " << ex.what() << "\n";
            return 1;
        }
    }

    sim.run();

    if (!snapshotOutPath.empty())
    {
        try
        {
            DemoSnapshot snapshot;
            snapshot.simState = canonicalizeSimState(sim.saveFrame());
            snapshot.throttle = thr;
            snapshot.force = force;
            snapshot.pos.assign(cars.pos.begin(), cars.pos.end());
            snapshot.vel.assign(cars.vel.begin(), cars.vel.end());
            snapshot.sparse.reserve(cars.sparse.size());
            snapshot.dense.reserve(cars.dense.size());
            for (std::size_t value : cars.sparse)
                snapshot.sparse.push_back(static_cast<std::uint64_t>(value));
            for (std::size_t value : cars.dense)
                snapshot.dense.push_back(static_cast<std::uint64_t>(value));
            storeSnapshotFile(snapshotOutPath, snapshot);
        }
        catch (const std::exception &ex)
        {
            std::cerr << "Snapshot write failed: " << ex.what() << "\n";
            return 1;
        }
    }

    if (metricsJson)
        std::cout << metricsRegistry.snapshot(metricsJsonInterval) << "\n";
    else
        std::cout << "Final pos0=" << cars.pos[0] << "\n";
    return 0;
}
