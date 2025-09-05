#pragma once
#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <cstdlib>

namespace fault {
inline std::atomic<double> delayProb{0.0};
inline std::atomic<int> maxDelayMs{0};
inline std::atomic<double> allocFailProb{0.0};
inline thread_local std::mt19937 rng{std::random_device{}()};

inline void set_delay_probability(double p){ delayProb.store(p); }
inline void set_max_delay_ms(int ms){ maxDelayMs.store(ms); }
inline void maybe_delay(){
    std::uniform_real_distribution<double> dist(0.0,1.0);
    if (dist(rng) < delayProb.load()){
        int ms = std::uniform_int_distribution<int>(0,maxDelayMs.load())(rng);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}
inline void set_alloc_failure_probability(double p){ allocFailProb.store(p); }
inline void* maybe_fail_alloc(std::size_t sz){
    std::uniform_real_distribution<double> dist(0.0,1.0);
    if (dist(rng) < allocFailProb.load()) return nullptr;
    return std::malloc(sz);
}
} // namespace fault
