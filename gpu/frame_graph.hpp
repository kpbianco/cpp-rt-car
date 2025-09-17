#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <thread>
#include <vector>

#include "hal/hal.hpp"
#include "hal/gpu_stub.hpp"
#include <rt/fiber_pool.hpp>

namespace simcore::hal::gpu {

// Simple timeline semaphore implemented with an atomic counter.
struct TimelineSemaphore {
    std::atomic<uint64_t> value{0};

    uint64_t signal(uint64_t inc = 1) {
        return value.fetch_add(inc, std::memory_order_release) + inc;
    }

    void wait(uint64_t target) {
        while (value.load(std::memory_order_acquire) < target) {
            std::this_thread::yield();
        }
    }

    uint64_t current() const {
        return value.load(std::memory_order_acquire);
    }
};

// Resource backed by pinned CPU memory to emulate zero-copy staging.
struct Resource {
    Buffer buf{nullptr, 0};
    std::size_t last_use = 0;
};

inline Resource allocate_resource(std::size_t bytes, hal::MemFlags flags = hal::MemFlags::pinned) {
    return Resource{Buffer{hal::alloc(bytes, flags), bytes}, 0};
}

struct OverlapBudget {
    hal::Duration cpu{0};
    hal::Duration gpu{0};
};

namespace detail {

inline rt::Task wait_for_fence_task(Fence fence, rt::FiberPool& pool) {
    co_await rt::FiberPool::FenceAwaiter{fence, pool};
    co_return;
}

} // namespace detail

class FrameGraph {
public:
    using PassFn = std::function<void()>;
    struct Pass { PassFn cpu; PassFn gpu; };

    FrameGraph() = default;
    explicit FrameGraph(rt::FiberPool* pool) : externalPool_(pool) {}

    void set_fiber_pool(rt::FiberPool* pool) { externalPool_ = pool; }

    ~FrameGraph() {
        for (auto& r : resources_) {
            if (r.buf.data) hal::free(r.buf.data);
        }
    }

    std::size_t create_resource(std::size_t bytes) {
        resources_.push_back(allocate_resource(bytes));
        return resources_.size() - 1;
    }

    Resource& resource(std::size_t idx) { return resources_[idx]; }

    std::size_t add_pass(PassFn cpu, PassFn gpu, std::initializer_list<std::size_t> uses = {}) {
        for (auto idx : uses) resources_[idx].last_use = passes_.size();
        passes_.push_back({std::move(cpu), std::move(gpu)});
        return passes_.size() - 1;
    }

    OverlapBudget execute() {
        OverlapBudget budget{};
        auto total_start = hal::now();
        std::atomic<hal::Duration::rep> gpu_total{0};
        rt::FiberPool* pool_ptr = nullptr;

        for (std::size_t i = 0; i < passes_.size(); ++i) {
            auto& p = passes_[i];
            Fence fence;
            if (p.gpu) {
                fence = submit([&, pass_idx = i]() {
                    auto start = hal::now();
                    cpu_timeline_.wait(pass_idx + 1);
                    p.gpu();
                    auto end = hal::now();
                    gpu_total.fetch_add(hal::elapsed(start, end).count(), std::memory_order_acq_rel);
                    gpu_timeline_.signal();
                });
            }
            bool cpu_signaled = false;
            if (p.cpu) {
                auto start = hal::now();
                p.cpu();
                budget.cpu += hal::elapsed(start, hal::now());
                cpu_timeline_.signal();
                cpu_signaled = true;
            }
            if (p.gpu && !cpu_signaled) {
                cpu_timeline_.signal();
                cpu_signaled = true;
            }
            if (p.gpu) {
                if (!pool_ptr)
                    pool_ptr = &ensure_pool();
                auto* pool = pool_ptr;
                pool->spawn(detail::wait_for_fence_task(std::move(fence), *pool));
            }
        }
        if (pool_ptr)
            pool_ptr->drain();
        for (std::size_t i = 0; i < passes_.size(); ++i)
            free_dead_resources(i);
        budget.gpu = hal::Duration{gpu_total.load(std::memory_order_acquire)};
        auto total = hal::elapsed(total_start, hal::now());
        auto raw = budget.cpu + budget.gpu - total;
        overlap_ = raw.count() > 0 ? raw : hal::Duration{0};
        return budget;
    }

    hal::Duration overlap() const { return overlap_; }
    TimelineSemaphore& cpu_timeline() { return cpu_timeline_; }
    TimelineSemaphore& gpu_timeline() { return gpu_timeline_; }

private:
    void free_dead_resources(std::size_t pass_idx) {
        for (auto& r : resources_) {
            if (r.buf.data && r.last_use == pass_idx) {
                hal::free(r.buf.data);
                r.buf.data = nullptr;
                r.buf.size = 0;
            }
        }
    }

    std::vector<Pass> passes_;
    std::vector<Resource> resources_;
    TimelineSemaphore cpu_timeline_;
    TimelineSemaphore gpu_timeline_;
    hal::Duration overlap_{0};
    rt::FiberPool* externalPool_{nullptr};
    std::unique_ptr<rt::FiberPool> ownedPool_;

    rt::FiberPool& ensure_pool() {
        if (externalPool_)
            return *externalPool_;
        if (!ownedPool_) {
            auto threads = std::thread::hardware_concurrency();
            if (threads == 0)
                threads = 1;
            ownedPool_ = std::make_unique<rt::FiberPool>(threads);
        }
        return *ownedPool_;
    }
};

// Stubs for mixed compute submissions.
template <typename Kernel>
inline Fence submit_spirv(const uint32_t* code, std::size_t words, Kernel&& kernel) {
    (void)code; (void)words;
    return submit(std::forward<Kernel>(kernel));
}

template <typename Kernel>
inline Fence submit_cuda(Kernel&& kernel) {
    return submit(std::forward<Kernel>(kernel));
}

} // namespace simcore::hal::gpu

