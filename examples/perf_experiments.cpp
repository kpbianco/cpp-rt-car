#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>
#ifdef __SSE__
#include <xmmintrin.h>
#endif

#include <simcore/logger.hpp>
#include <simcore/profiler.hpp>

using clock_type = std::chrono::steady_clock;

static Logger* g_log = nullptr;
static Profiler* g_prof = nullptr;
static std::size_t g_scale = 1;

// Simple micro benchmark helpers that feed our logger/profiler
static void log_time(const char* name, clock_type::duration d)
{
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(d).count();
    if (g_log)
        LOG_INFO(g_log, "{}: {} us", name, us);
    if (g_prof)
        g_prof->setCounter(name, static_cast<long double>(us));
}

// -----------------------------------------------------------------------------
// pairwise vs kahan reduction
// -----------------------------------------------------------------------------

static double kahan_sum(const std::vector<double>& v)
{
    double sum = 0.0;
    double c = 0.0;
    for (double x : v) {
        double y = x - c;
        double t = sum + y;
        c = (t - sum) - y;
        sum = t;
    }
    return sum;
}

static double pairwise_sum(const double* begin, const double* end)
{
    auto len = end - begin;
    if (len <= 1)
        return len ? *begin : 0.0;
    const double* mid = begin + len / 2;
    return pairwise_sum(begin, mid) + pairwise_sum(mid, end);
}

static void experiment_pairwise_vs_kahan()
{
    PROF_SCOPE(g_prof, "pairwise_vs_kahan");
    std::vector<double> data((1 << 20) * g_scale, 1.0 / 3.0);
    auto start = clock_type::now();
    volatile double naive = std::accumulate(data.begin(), data.end(), 0.0);
    auto t_naive = clock_type::now() - start;
    start = clock_type::now();
    volatile double kahan = kahan_sum(data);
    auto t_kahan = clock_type::now() - start;
    start = clock_type::now();
    volatile double pairwise = pairwise_sum(data.data(), data.data() + data.size());
    auto t_pair = clock_type::now() - start;

    if (g_log)
        LOG_INFO(g_log, "Reduction accuracy check: naive={} kahan={} pairwise={}",
                 naive, kahan, pairwise);
    log_time("naive reduction", t_naive);
    log_time("kahan reduction", t_kahan);
    log_time("pairwise reduction", t_pair);
}

// -----------------------------------------------------------------------------
// FTZ/DAZ impact
// -----------------------------------------------------------------------------
#ifdef __SSE__
static void experiment_ftz_daz()
{
    PROF_SCOPE(g_prof, "ftz_daz");
    std::vector<float> data((1 << 20) * g_scale, std::numeric_limits<float>::denorm_min());
    auto run = [&](bool enable) {
        unsigned int csr = _mm_getcsr();
        if (enable)
            _mm_setcsr(csr | 0x8040); // FTZ | DAZ
        else
            _mm_setcsr(csr & ~0x8040);
        auto start = clock_type::now();
        float s = 0.0f;
        for (float x : data)
            s += std::sqrt(x);
        auto dur = clock_type::now() - start;
        _mm_setcsr(csr);
        return dur;
    };

    auto t_off = run(false);
    auto t_on = run(true);
    log_time("FTZ/DAZ off", t_off);
    log_time("FTZ/DAZ on", t_on);
}
#else
static void experiment_ftz_daz()
{
    if (g_log)
        LOG_WARN(g_log, "FTZ/DAZ experiment requires SSE support.");
}
#endif

// -----------------------------------------------------------------------------
// pre-touch vs demand-fault
// -----------------------------------------------------------------------------
static void experiment_pretouch_vs_demand()
{
    PROF_SCOPE(g_prof, "pretouch_vs_demand");
    const size_t pages = 64 * 1024 * g_scale; // 256 MB on 4k pages
    const size_t bytes = pages * 4096;
    char* buf = static_cast<char*>(std::malloc(bytes));
    if (!buf) {
        if (g_log)
            LOG_ERROR(g_log, "allocation failed");
        return;
    }

    auto touch = [&](char* p) {
        for (size_t i = 0; i < bytes; i += 4096)
            p[i] = 1;
    };

    auto start = clock_type::now();
    touch(buf); // demand-fault first touch
    auto t_first = clock_type::now() - start;
    start = clock_type::now();
    touch(buf); // pages are warm now
    auto t_second = clock_type::now() - start;
    std::free(buf);

    log_time("demand fault", t_first);
    log_time("pre-touched", t_second);
}

// -----------------------------------------------------------------------------
// snapshot cost
// -----------------------------------------------------------------------------
static void experiment_snapshot_cost()
{
    PROF_SCOPE(g_prof, "snapshot_cost");
    const size_t bytes = 128 * 1024 * 1024 * g_scale; // 128 MB
    std::vector<char> src(bytes, 1);
    std::vector<char> dst(bytes, 0);

    auto start = clock_type::now();
    std::memcpy(dst.data(), src.data(), bytes);
    auto t_copy = clock_type::now() - start;
    log_time("128MB memcpy", t_copy);
}

// -----------------------------------------------------------------------------
// huge page on/off
// -----------------------------------------------------------------------------
#ifdef __linux__
#include <sys/mman.h>
static void experiment_huge_pages()
{
    PROF_SCOPE(g_prof, "huge_pages");
    const size_t bytes = 128 * 1024 * 1024 * g_scale; // 128 MB
    auto measure = [&](bool huge) {
        void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | (huge ? MAP_HUGETLB : 0), -1, 0);
        if (p == MAP_FAILED) {
            return clock_type::duration::max();
        }
        auto start = clock_type::now();
        std::memset(p, 0, bytes);
        auto dur = clock_type::now() - start;
        munmap(p, bytes);
        return dur;
    };

    auto t_norm = measure(false);
    auto t_huge = measure(true);
    if (t_norm == clock_type::duration::max() ||
        t_huge == clock_type::duration::max()) {
        if (g_log)
            LOG_WARN(g_log, "huge pages not available (need privileges and kernel support)");
        return;
    }
    log_time("normal pages", t_norm);
    log_time("huge pages", t_huge);
}
#else
static void experiment_huge_pages()
{
    if (g_log)
        LOG_WARN(g_log, "huge page experiment only implemented on Linux");
}
#endif

// -----------------------------------------------------------------------------
// AoSoA block sizes (very rough)
// -----------------------------------------------------------------------------
static void experiment_aosoa_blocks()
{
    PROF_SCOPE(g_prof, "aosoa_blocks");
    size_t elements = (1 << 20) * g_scale;
    auto run = [&](size_t block) {
        size_t blocks = (elements + block - 1) / block;
        std::vector<float> x(elements, 1.0f), y(elements, 2.0f);
        auto start = clock_type::now();
        for (size_t b = 0; b < blocks; ++b) {
            size_t base = b * block;
            size_t end = std::min(base + block, elements);
            for (size_t i = base; i < end; ++i)
                x[i] += y[i];
        }
        return clock_type::now() - start;
    };
    auto t64 = run(64);
    auto t256 = run(256);
    log_time("AoSoA block 64", t64);
    log_time("AoSoA block 256", t256);
}

// -----------------------------------------------------------------------------
// work-steal vs global queue (simplified)
// -----------------------------------------------------------------------------
static void experiment_worksteal_vs_global()
{
    PROF_SCOPE(g_prof, "worksteal_vs_global");
    const int threads = std::thread::hardware_concurrency();
    const int tasks = threads * 10000 * static_cast<int>(g_scale);
    auto work = [](int) {
        volatile double x = 0;
        for (int i = 0; i < 100; ++i)
            x += std::sin(i);
    };

    auto run_global = [&]() {
        std::vector<std::thread> ths;
        std::atomic<int> index{0};
        auto start = clock_type::now();
        for (int t = 0; t < threads; ++t) {
            ths.emplace_back([&]() {
                int i;
                while ((i = index.fetch_add(1)) < tasks)
                    work(i);
            });
        }
        for (auto& th : ths)
            th.join();
        return clock_type::now() - start;
    };

    auto t_global = run_global();
    // very rough work-stealing: each thread processes its chunk then steals
    auto run_steal = [&]() {
        std::vector<std::thread> ths;
        std::vector<int> next(tasks, 0);
        std::atomic<int> chunk{0};
        auto start = clock_type::now();
        for (int t = 0; t < threads; ++t) {
            ths.emplace_back([&]() {
                for (;;) {
                    int i = chunk.fetch_add(1);
                    if (i >= tasks)
                        break;
                    work(i);
                }
            });
        }
        for (auto& th : ths)
            th.join();
        return clock_type::now() - start;
    };
    auto t_steal = run_steal();
    log_time("global queue", t_global);
    log_time("work stealing", t_steal);
}

// -----------------------------------------------------------------------------
// NUMA cross-node penalties (placeholder)
// -----------------------------------------------------------------------------
static void experiment_numa_penalties()
{
    if (g_log)
        LOG_WARN(g_log, "NUMA experiment requires libnuma and multiple nodes; not run here.");
}

// -----------------------------------------------------------------------------
// fiber vs thread blocking (placeholder)
// -----------------------------------------------------------------------------
static void experiment_fiber_vs_thread()
{
    if (g_log)
        LOG_WARN(g_log, "Fiber vs thread blocking experiment not implemented (requires fiber lib).");
}

int main(int argc, char** argv)
{
    Logger logger;
    logger.addSink(std::make_shared<Logger::StdoutSink>());
    logger.setLevel(Logger::Level::Info);
    Profiler profiler;
    g_log = &logger;
    g_prof = &profiler;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg.rfind("--scale=", 0) == 0) {
            g_scale = std::stoull(std::string(arg.substr(8)));
        }
    }

    experiment_numa_penalties();
    experiment_huge_pages();
    experiment_aosoa_blocks();
    experiment_worksteal_vs_global();
    experiment_fiber_vs_thread();
    experiment_ftz_daz();
    experiment_pairwise_vs_kahan();
    experiment_pretouch_vs_demand();
    experiment_snapshot_cost();

    for (auto &c : profiler.counters()) {
        LOG_INFO(&logger, "counter {} = {}", c.name, c.value);
    }
    profiler.dump();
    return 0;
}
