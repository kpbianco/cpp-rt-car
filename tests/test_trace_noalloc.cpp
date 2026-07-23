#include <gtest/gtest.h>

#include <rt/runtime.hpp>
#include <simcore/bintrace.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace {
std::atomic<std::size_t> g_allocation_count{0};
std::atomic<bool> g_track_allocations{false};

void record_allocation() noexcept {
    if (g_track_allocations.load(std::memory_order_relaxed)) {
        g_allocation_count.fetch_add(1, std::memory_order_relaxed);
    }
}

std::size_t align_up(std::size_t value, std::size_t alignment) {
    const std::size_t mask = alignment - 1;
    const std::size_t rounded = (value + mask) & ~mask;
    if (rounded < value) {
        throw std::bad_alloc();
    }
    return rounded;
}

void* allocate_raw(std::size_t size) {
    if (size == 0) size = 1;
    if (void* ptr = std::malloc(size)) {
        return ptr;
    }
    throw std::bad_alloc();
}

void* allocate_aligned(std::size_t size, std::size_t alignment) {
    if (alignment < alignof(std::max_align_t)) {
        alignment = alignof(std::max_align_t);
    }
    if (size == 0) {
        size = alignment;
    }
    size = align_up(size, alignment);
#if defined(_MSC_VER)
    if (void* ptr = _aligned_malloc(size, alignment)) {
        return ptr;
    }
    throw std::bad_alloc();
#else
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) == 0) {
        return ptr;
    }
    throw std::bad_alloc();
#endif
}

void deallocate_aligned(void* ptr) noexcept {
#if defined(_MSC_VER)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}

struct AllocationGuard {
    AllocationGuard() {
        g_allocation_count.store(0, std::memory_order_relaxed);
        g_track_allocations.store(true, std::memory_order_release);
    }
    ~AllocationGuard() {
        g_track_allocations.store(false, std::memory_order_release);
    }
};

std::size_t allocation_count() {
    return g_allocation_count.load(std::memory_order_acquire);
}

struct RuntimeAllocationProbe {
    std::uint32_t value = 0;
    std::array<std::uint32_t, 2>* execution = nullptr;
    std::size_t* count = nullptr;
};

rt::CallbackResult record_runtime_phase(
    void* user_data,
    const rt::CallbackContext&) {
    auto& probe = *static_cast<RuntimeAllocationProbe*>(user_data);
    (*probe.execution)[(*probe.count)++] = probe.value;
    return rt::CallbackResult::ok;
}

} // namespace

void* operator new(std::size_t size) {
    record_allocation();
    return allocate_raw(size);
}

void* operator new[](std::size_t size) {
    record_allocation();
    return allocate_raw(size);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    record_allocation();
    return allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    record_allocation();
    return allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new[](size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size, alignment);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, std::align_val_t alignment,
                      const std::nothrow_t&) noexcept {
    try {
        return ::operator new[](size, alignment);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* ptr) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::align_val_t alignment) noexcept {
    (void)alignment;
    if (!ptr) return;
    deallocate_aligned(ptr);
}

void operator delete[](void* ptr, std::align_val_t alignment) noexcept {
    (void)alignment;
    if (!ptr) return;
    deallocate_aligned(ptr);
}

void operator delete(void* ptr, std::size_t, std::align_val_t alignment) noexcept {
    (void)alignment;
    if (!ptr) return;
    deallocate_aligned(ptr);
}

void operator delete[](void* ptr, std::size_t, std::align_val_t alignment) noexcept {
    (void)alignment;
    if (!ptr) return;
    deallocate_aligned(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    (void)alignment;
    if (!ptr) return;
    deallocate_aligned(ptr);
}

void operator delete[](void* ptr, std::align_val_t alignment,
                       const std::nothrow_t&) noexcept {
    (void)alignment;
    if (!ptr) return;
    deallocate_aligned(ptr);
}

void operator delete(void* ptr, std::size_t, const std::nothrow_t&) noexcept {
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t, const std::nothrow_t&) noexcept {
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    (void)alignment;
    if (!ptr) return;
    deallocate_aligned(ptr);
}

void operator delete[](void* ptr, std::size_t, std::align_val_t alignment,
                       const std::nothrow_t&) noexcept {
    (void)alignment;
    if (!ptr) return;
    deallocate_aligned(ptr);
}

TEST(TraceNoAlloc, LogHotPathDoesNotAllocate) {
    using namespace bintrace;

    Trace trace;
    constexpr std::size_t kThreads = 1;
    constexpr std::size_t kEventsPerThread = 1u << 12;
    trace.init(kThreads, kEventsPerThread, true);
    trace.bindThread(0);

    constexpr std::size_t kBurst = 256;

    {
        AllocationGuard guard;
        for (std::size_t i = 0; i < kBurst; ++i) {
            trace.log(EV_PhaseBegin, static_cast<std::uint32_t>(i), i);
        }
    }

    const std::size_t allocations = allocation_count();
    EXPECT_EQ(0u, allocations);
    EXPECT_EQ(0u, trace.dropped());

    auto snap = trace.snapshot();
    ASSERT_EQ(1u, snap.perThreadCount.size());
    EXPECT_EQ(kBurst, snap.perThreadCount[0]);
    EXPECT_EQ(kBurst, snap.events.size());

    for (std::size_t i = 0; i < kBurst; ++i) {
        const auto& ev = snap.events[i];
        EXPECT_EQ(EV_PhaseBegin, ev.code);
        EXPECT_EQ(static_cast<std::uint32_t>(i), ev.a);
        EXPECT_EQ(static_cast<std::uint64_t>(i), ev.b);
        EXPECT_EQ(0u, ev.thread);
    }

    trace.shutdown();
}

TEST(TraceNoAlloc, CompiledGraphFirstFrameDoesNotAllocate) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 2;
    config.scratch_bytes = 64;
    config.trace_capacity = 16;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);

    std::array<std::uint32_t, 2> execution{};
    std::size_t execution_count = 0;
    RuntimeAllocationProbe consumer_probe{0, &execution, &execution_count};
    RuntimeAllocationProbe producer_probe{1, &execution, &execution_count};
    rt::PhaseHandle consumer;
    rt::PhaseHandle producer;
    rt::ResourceHandle state;
    ASSERT_EQ(
        runtime.register_callback(
            {"consumer", &record_runtime_phase, &consumer_probe},
            consumer),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback(
            {"producer", &record_runtime_phase, &producer_probe},
            producer),
        rt::Status::ok);
    ASSERT_EQ(runtime.register_resource("state", state), rt::Status::ok);
    ASSERT_EQ(runtime.add_dependency(producer, consumer), rt::Status::ok);
    ASSERT_EQ(
        runtime.declare_resource_access(
            producer,
            state,
            rt::ResourceAccess::write),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.declare_resource_access(
            consumer,
            state,
            rt::ResourceAccess::read),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    rt::Status step_status = rt::Status::internal_error;
    {
        AllocationGuard guard;
        step_status = runtime.step(
            rt::HostFrameContext{
                0,
                std::chrono::nanoseconds(1),
                std::nullopt,
            });
    }

    EXPECT_EQ(step_status, rt::Status::ok);
    EXPECT_EQ(allocation_count(), 0u);
    EXPECT_EQ(execution_count, 2u);
    EXPECT_EQ(execution[0], 1u);
    EXPECT_EQ(execution[1], 0u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}
