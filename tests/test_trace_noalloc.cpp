#include <gtest/gtest.h>

#include <rt/runtime.hpp>
#include <simcore/bintrace.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <span>

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

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void deallocate_raw(void* ptr) noexcept {
    std::free(ptr);
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

struct FullFramePhaseState {
    std::uint64_t seed = 0;
    std::uint64_t frame = 0;
    std::uint64_t total = 0;
    std::array<std::uint64_t, 64> values{};
    std::array<std::uint64_t, 8> partials{};
    std::atomic<std::size_t> errors{0};
};

bool valid_task_scratch(const rt::TaskContext& context) {
    const auto scratch = context.scratch();
    return scratch.size() == 64 &&
        scratch.data() != nullptr &&
        (reinterpret_cast<std::uintptr_t>(scratch.data()) % 64u) == 0u;
}

rt::TaskResult fill_full_frame_range(
    void* user_data,
    const rt::TaskContext& context,
    const rt::TaskRange& range) {
    auto& state = *static_cast<FullFramePhaseState*>(user_data);
    if (!valid_task_scratch(context)) {
        state.errors.fetch_add(1, std::memory_order_relaxed);
        return rt::TaskResult::error;
    }
    context.scratch().front() = std::byte{0x3c};
    for (std::size_t index = range.begin; index < range.end; ++index) {
        state.values[index] = state.seed + state.frame + index;
    }
    return rt::TaskResult::ok;
}

rt::TaskResult reduce_full_frame_range(
    void* user_data,
    const rt::TaskContext& context,
    const rt::TaskRange& range) {
    auto& state = *static_cast<FullFramePhaseState*>(user_data);
    if (!valid_task_scratch(context) ||
        range.task_index >= state.partials.size()) {
        state.errors.fetch_add(1, std::memory_order_relaxed);
        return rt::TaskResult::error;
    }
    std::uint64_t partial = 0;
    for (std::size_t index = range.begin; index < range.end; ++index) {
        partial += state.values[index];
    }
    state.partials[range.task_index] = partial;
    return rt::TaskResult::ok;
}

rt::TaskResult combine_full_frame_range(
    void* user_data,
    const rt::TaskContext& context,
    std::size_t left,
    std::size_t right) {
    auto& state = *static_cast<FullFramePhaseState*>(user_data);
    if (!valid_task_scratch(context) ||
        left >= state.partials.size() ||
        right >= state.partials.size()) {
        state.errors.fetch_add(1, std::memory_order_relaxed);
        return rt::TaskResult::error;
    }
    state.partials[left] += state.partials[right];
    return rt::TaskResult::ok;
}

rt::CallbackResult run_full_frame_phase(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& state = *static_cast<FullFramePhaseState*>(user_data);
    if (context.scratch.size() != 128 ||
        context.scratch.data() == nullptr ||
        (reinterpret_cast<std::uintptr_t>(
             context.scratch.data()) % 64u) != 0u ||
        !valid_task_scratch(context.tasks)) {
        state.errors.fetch_add(1, std::memory_order_relaxed);
        return rt::CallbackResult::error;
    }

    context.scratch.front() = std::byte{0x5a};
    state.frame = context.frame.frame_index;
    if (context.tasks.parallel_for(
            state.values.size(),
            8,
            &fill_full_frame_range,
            &state) != rt::Status::ok ||
        context.tasks.parallel_reduce(
            state.values.size(),
            8,
            &reduce_full_frame_range,
            &combine_full_frame_range,
            &state) != rt::Status::ok) {
        state.errors.fetch_add(1, std::memory_order_relaxed);
        return rt::CallbackResult::error;
    }
    state.total = state.partials[0];
    return rt::CallbackResult::ok;
}

struct FullFrameSinkState {
    FullFramePhaseState* first = nullptr;
    FullFramePhaseState* second = nullptr;
    std::uint64_t calls = 0;
    std::size_t errors = 0;
};

std::uint64_t expected_full_frame_total(
    std::uint64_t seed,
    std::uint64_t frame) {
    constexpr std::uint64_t kIndexSum = (63u * 64u) / 2u;
    return 64u * (seed + frame) + kIndexSum;
}

rt::CallbackResult validate_full_frame(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& sink = *static_cast<FullFrameSinkState*>(user_data);
    if (!sink.first || !sink.second ||
        sink.first->total != expected_full_frame_total(
            sink.first->seed,
            context.frame.frame_index) ||
        sink.second->total != expected_full_frame_total(
            sink.second->seed,
            context.frame.frame_index)) {
        ++sink.errors;
        return rt::CallbackResult::error;
    }
    ++sink.calls;
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
    deallocate_raw(ptr);
}

void operator delete[](void* ptr) noexcept {
    deallocate_raw(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept {
    deallocate_raw(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept {
    deallocate_raw(ptr);
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
    deallocate_raw(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    deallocate_raw(ptr);
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
    deallocate_raw(ptr);
}

void operator delete[](void* ptr, std::size_t, const std::nothrow_t&) noexcept {
    deallocate_raw(ptr);
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

TEST(TraceNoAlloc, CheckpointAndInputCodecDoNotAllocate) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 1;
    config.trace_capacity = 8;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    config.memory_budget_bytes = 1024 * 1024;
    config.determinism_tier =
        rt::DeterminismTier::schedule_independent;
    config.state_capacity = 1;
    config.snapshot_max_bytes = 1024;
    config.replay_input_capacity = 1;
    config.input_log_max_bytes = 1024;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);

    std::array<std::byte, 16> state{};
    ASSERT_EQ(
        runtime.register_state(
            {"state", 1, state}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);

    std::array<std::byte, 1024> checkpoint{};
    std::array<std::byte, 1024> input_log{};
    std::array<std::byte, 8> payload{};
    const std::array records{
        rt::ReplayInputRecord{
            rt::HostFrameContext{
                1,
                std::chrono::nanoseconds(1),
                std::nullopt,
            },
            1,
            payload,
        },
    };
    rt::ArtifactWriteResult checkpoint_result;
    rt::ArtifactWriteResult input_result;
    rt::CheckpointMetadata checkpoint_metadata;
    rt::InputLogMetadata input_metadata;
    rt::Status checkpoint_status = rt::Status::internal_error;
    rt::Status checkpoint_inspect_status =
        rt::Status::internal_error;
    rt::Status restore_status = rt::Status::internal_error;
    rt::Status input_status = rt::Status::internal_error;
    rt::Status input_inspect_status =
        rt::Status::internal_error;

    {
        AllocationGuard guard;
        checkpoint_status = runtime.write_checkpoint(
            0,
            checkpoint,
            checkpoint_result);
        checkpoint_inspect_status =
            rt::inspect_checkpoint_artifact(
                std::span<const std::byte>(
                    checkpoint.data(),
                    checkpoint_result.bytes_written),
                checkpoint_metadata);
        state.fill(std::byte{0x5a});
        restore_status = runtime.restore_checkpoint(
            std::span<const std::byte>(
                checkpoint.data(),
                checkpoint_result.bytes_written));
        input_status = runtime.write_input_log(
            records,
            input_log,
            input_result);
        input_inspect_status =
            rt::inspect_input_log_artifact(
                std::span<const std::byte>(
                    input_log.data(),
                    input_result.bytes_written),
                input_metadata);
    }

    EXPECT_EQ(checkpoint_status, rt::Status::ok);
    EXPECT_EQ(checkpoint_inspect_status, rt::Status::ok);
    EXPECT_EQ(restore_status, rt::Status::ok);
    EXPECT_EQ(input_status, rt::Status::ok);
    EXPECT_EQ(input_inspect_status, rt::Status::ok);
    EXPECT_EQ(allocation_count(), 0u);
    EXPECT_TRUE(std::all_of(
        state.begin(),
        state.end(),
        [](std::byte value) {
            return value == std::byte{0};
        }));
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

void run_complete_cpu_frames_noalloc(rt::ExecutorPolicy policy) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 3;
    config.scratch_bytes = 128;
    config.trace_capacity = 256;
    config.executor_policy = policy;
    config.worker_count = 4;
    config.executor_queue_capacity = 64;
    config.scratch_alignment = 64;
    config.task_scratch_bytes = 64;
    config.task_scratch_slots = 64;
    config.memory_budget_bytes = 1024 * 1024;
    // Exercise watchdog arm/disarm inside the measured frame path while
    // keeping the service-lane deadline comfortably beyond the test.
    config.watchdog_timeout_ns = 60'000'000'000ull;
    config.watchdog_max_degradation_level = 2;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);

    FullFramePhaseState first;
    first.seed = 11;
    FullFramePhaseState second;
    second.seed = 101;
    FullFrameSinkState sink{&first, &second};
    rt::PhaseHandle first_phase;
    rt::PhaseHandle second_phase;
    rt::PhaseHandle sink_phase;
    ASSERT_EQ(
        runtime.register_callback(
            {"full-frame.first", &run_full_frame_phase, &first},
            first_phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback(
            {"full-frame.second", &run_full_frame_phase, &second},
            second_phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback(
            {"full-frame.sink", &validate_full_frame, &sink},
            sink_phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.add_dependency(first_phase, sink_phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.add_dependency(second_phase, sink_phase),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    constexpr std::uint64_t kFrames = 64;
    rt::Status step_status = rt::Status::ok;
    rt::StepResult step_result;
    {
        AllocationGuard guard;
        for (std::uint64_t frame = 0; frame < kFrames; ++frame) {
            step_status = runtime.step(
                rt::HostFrameContext{
                    frame,
                    std::chrono::nanoseconds(1),
                    std::nullopt,
                },
                &step_result);
            if (step_status != rt::Status::ok) {
                break;
            }
        }
    }

    EXPECT_EQ(step_status, rt::Status::ok);
    EXPECT_EQ(allocation_count(), 0u);
    EXPECT_FALSE(step_result.watchdog_fired);
    EXPECT_EQ(step_result.degradation_level, 0u);
    EXPECT_EQ(first.errors.load(), 0u);
    EXPECT_EQ(second.errors.load(), 0u);
    EXPECT_EQ(sink.errors, 0u);
    EXPECT_EQ(sink.calls, kFrames);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(TraceNoAlloc, CompleteCpuFramesDoNotAllocate) {
    run_complete_cpu_frames_noalloc(
        rt::ExecutorPolicy::bounded_throughput);
}

TEST(TraceNoAlloc, StaticCompleteCpuFramesDoNotAllocate) {
    run_complete_cpu_frames_noalloc(
        rt::ExecutorPolicy::static_deterministic);
}
