#pragma once
#include <string>
#include <vector>
#include <chrono>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <iostream>
#include <iomanip>

#ifdef PROF_ENABLED

class Profiler {
public:
    struct Entry {
        std::string name;
        std::uint64_t count = 0;
        long double totalNs = 0;
        long double minNs = 0;
        long double maxNs = 0;
    };

    class ScopeGuard {
    public:
        ScopeGuard(Profiler* p, std::string n)
            : prof_(p), name_(std::move(n)),
              t0_(std::chrono::steady_clock::now()) {}
        ~ScopeGuard() {
            if (!prof_) return;
            auto t1 = std::chrono::steady_clock::now();
            long double ns = std::chrono::duration<long double, std::nano>(t1 - t0_).count();
            prof_->record(name_, ns);
        }
    private:
        Profiler* prof_;
        std::string name_;
        std::chrono::steady_clock::time_point t0_;
    };

    void record(const std::string& n, long double ns) {
        std::lock_guard<std::mutex> lk(m_);
        auto &e = map_[n];
        if (e.count == 0) {
            e.name = n;
            e.minNs = e.maxNs = ns;
        } else {
            if (ns < e.minNs) e.minNs = ns;
            if (ns > e.maxNs) e.maxNs = ns;
        }
        e.totalNs += ns;
        ++e.count;
    }

    std::vector<Entry> summary() const {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<Entry> out;
        out.reserve(map_.size());
        for (auto &kv : map_) out.push_back(kv.second);
        std::sort(out.begin(), out.end(),
                  [](auto& a, auto& b){ return a.name < b.name; });
        return out;
    }

    void dump() {
        auto rows = summary();
        if (rows.empty()) return;
        std::cout << "\n==== Profiler Summary (ns / µs / ms) ====\n";
        std::cout << std::left << std::setw(40) << "Section"
                  << std::right << std::setw(12) << "Count"
                  << std::setw(14) << "Avg (µs)"
                  << std::setw(15) << "Total (ms)"
                  << std::setw(14) << "Min (µs)"
                  << std::setw(14) << "Max (µs)\""
                  << "\n";
        for (auto &e : rows) {
            long double avg = e.totalNs / (e.count ? e.count : 1);
            auto toUs = [](long double ns){ return ns / 1000.0L; };
            auto toMs = [](long double ns){ return ns / 1'000'000.0L; };
            std::cout << std::left << std::setw(40) << e.name
                      << std::right << std::setw(12) << e.count
                      << std::setw(14) << std::fixed << std::setprecision(3) << toUs(avg)
                      << std::setw(15) << std::fixed << std::setprecision(3) << toMs(e.totalNs)
                      << std::setw(14) << std::fixed << std::setprecision(3) << toUs(e.minNs)
                      << std::setw(14) << std::fixed << std::setprecision(3) << toUs(e.maxNs)
                      << "\n";
        }
        std::cout << "=========================================\n";
    }

private:
    mutable std::mutex m_;
    std::unordered_map<std::string, Entry> map_;
};

#define PROF_CONCAT_INNER(a,b) a##b
#define PROF_CONCAT(a,b) PROF_CONCAT_INNER(a,b)

// Null-safe & unique-name scope macro
#define PROF_SCOPE(PTR, NAME) \
    auto PROF_CONCAT(_prof_enabled_, __LINE__) = (PTR); \
    if (PROF_CONCAT(_prof_enabled_, __LINE__)) \
        ::Profiler::ScopeGuard PROF_CONCAT(_prof_guard_, __LINE__){PROF_CONCAT(_prof_enabled_, __LINE__), (NAME)}; \
    else (void)0

#else   // PROF_ENABLED not defined

class Profiler {
public:
    struct Entry { std::string name; std::uint64_t count=0; long double totalNs=0,minNs=0,maxNs=0; };
    class ScopeGuard { public: ScopeGuard(Profiler*, std::string) {} };
    std::vector<Entry> summary() const { return {}; }
    void dump() {}
};

#define PROF_SCOPE(PTR, NAME) do{}while(0)

#endif
