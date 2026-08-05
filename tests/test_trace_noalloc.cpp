#include <gtest/gtest.h>

#include <rt/cuda_backend.hpp>
#include <rt/runtime.hpp>
#include <rt/mock_device.hpp>
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

struct NoAllocMemoryProvider {
    struct alignas(64) Slot {
        std::array<std::byte, 64 * 1024> bytes{};
    };

    rt::MemoryProvider table() noexcept {
        rt::MemoryProvider provider;
        provider.capabilities =
            rt::memory_provider_capability_bit(
                rt::MemoryProviderCapability::policy_operations) |
            rt::memory_provider_capability_bit(
                rt::MemoryProviderCapability::independent_observation);
        provider.user_data = this;
        provider.acquire = &acquire;
        provider.apply = &apply;
        provider.observe = &observe;
        provider.rollback = &rollback;
        provider.release = &release;
        return provider;
    }

    std::array<Slot, 3> slots{};

    static rt::Status acquire(
        void* user_data,
        const rt::MemoryProviderAcquireRequest& request,
        rt::MemoryProviderAllocation& allocation) noexcept {
        auto& self = *static_cast<NoAllocMemoryProvider*>(user_data);
        const auto index = static_cast<std::size_t>(
            request.region.value - rt::memory_region_phase_scratch.value);
        if (index >= self.slots.size() ||
            request.logical_bytes > self.slots[index].bytes.size() ||
            request.required_alignment > alignof(Slot)) {
            return rt::Status::resource_exhausted;
        }
        auto& slot = self.slots[index];
        allocation.token = &slot;
        allocation.allocation_base = slot.bytes.data();
        allocation.allocation_bytes = request.logical_bytes;
        allocation.usable_data = slot.bytes.data();
        allocation.usable_bytes = request.logical_bytes;
        allocation.committed_bytes = request.logical_bytes;
        allocation.alignment = alignof(Slot);
        return rt::Status::ok;
    }

    static rt::Status apply(
        void*,
        void*,
        const rt::MemoryPolicy&,
        rt::MemoryProviderObservation& applied) noexcept {
        applied.independently_observed = true;
        return rt::Status::ok;
    }

    static rt::Status observe(
        void*,
        void*,
        const rt::MemoryPolicy&,
        rt::MemoryProviderObservation& observed) noexcept {
        observed.independently_observed = true;
        return rt::Status::ok;
    }

    static rt::Status rollback(
        void*,
        void*,
        const rt::MemoryPolicy&,
        const rt::MemoryProviderObservation&) noexcept {
        return rt::Status::ok;
    }

    static void release(void*, void*, rt::RollbackIntent) noexcept {}
};

void run_complete_cpu_frames_noalloc(
    rt::ExecutorPolicy policy,
    bool provider_backed = false) {
    NoAllocMemoryProvider memory_provider;
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
    if (provider_backed) {
        ASSERT_EQ(
            runtime.set_memory_provider(memory_provider.table()),
            rt::Status::ok);
    }

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

TEST(TraceNoAlloc, ProviderBackedCompleteCpuFramesDoNotAllocate) {
    run_complete_cpu_frames_noalloc(
        rt::ExecutorPolicy::bounded_throughput,
        true);
}

namespace {

rt::CallbackResult submit_noalloc_device_command(
    void*,
    const rt::DeviceCallbackContext&,
    rt::DeviceSubmission& submission) {
    submission.timeout_ns = 1'000'000;
    submission.opcode = rt::mock_device_opcode_noop;
    return rt::CallbackResult::ok;
}

} // namespace

void run_complete_device_frames_noalloc(
    bool provider_backed,
    bool explicit_rate_plan = false) {
    NoAllocMemoryProvider memory_provider;
    rt::MockDeviceBackend backend({
        8,
        1,
        1,
        1'000,
    });
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 1;
    config.trace_capacity = 256;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    config.device_backend_capacity = 1;
    config.device_buffer_capacity = 1;
    config.device_outstanding_capacity = 8;
    config.device_completion_batch = 4;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    if (provider_backed) {
        ASSERT_EQ(
            runtime.set_memory_provider(memory_provider.table()),
            rt::Status::ok);
    }

    rt::DeviceBackendHandle backend_handle;
    ASSERT_EQ(
        runtime.register_device_backend(
            {"mock", backend.api()},
            backend_handle),
        rt::Status::ok);
    rt::PhaseHandle phase;
    ASSERT_EQ(
        runtime.register_device_phase(
            {
                "device.noalloc",
                backend_handle,
                &submit_noalloc_device_command,
                nullptr,
            },
            phase),
        rt::Status::ok);
    if (explicit_rate_plan) {
        rt::RateDomainHandle domain;
        ASSERT_EQ(
            runtime.register_rate_domain(
                {"device.rate", 2, 2, 1, 1},
                domain),
            rt::Status::ok);
        ASSERT_EQ(
            runtime.bind_phase_to_rate_domain(phase, domain),
            rt::Status::ok);
    }
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(
        runtime.step({0, std::chrono::nanoseconds(1), std::nullopt}),
        rt::Status::ok);

    rt::Status status = rt::Status::ok;
    {
        AllocationGuard guard;
        for (std::uint64_t frame = 1; frame <= 64; ++frame) {
            status = runtime.step({
                frame,
                std::chrono::nanoseconds(1),
                std::nullopt,
            });
            if (status != rt::Status::ok) {
                break;
            }
        }
    }
    EXPECT_EQ(status, rt::Status::ok);
    EXPECT_EQ(allocation_count(), 0u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(TraceNoAlloc, CompleteDeviceFramesDoNotAllocate) {
    run_complete_device_frames_noalloc(false);
}

TEST(TraceNoAlloc, ProviderBackedCompleteDeviceFramesDoNotAllocate) {
    run_complete_device_frames_noalloc(true);
}

TEST(TraceNoAlloc, RatePlanCompleteDeviceFramesDoNotAllocate) {
    run_complete_device_frames_noalloc(false, true);
}

namespace {

struct NoAllocCudaDriver {
    static constexpr rt::CudaContext context = 0xc001u;

    rt::CudaDriverApi api() noexcept {
        rt::CudaDriverApi result{};
        result.user_data = this;
        result.push_context = &push_context;
        result.pop_context = &pop_context;
        result.event_create = &event_create;
        result.event_destroy = &event_operation;
        result.event_record = &event_record;
        result.event_query = &event_operation;
        result.event_synchronize = &event_operation;
        result.stream_synchronize = &stream_operation;
        result.mem_alloc = &mem_alloc;
        result.mem_free = &mem_free;
        result.host_register = &host_register;
        result.host_unregister = &host_unregister;
        result.memcpy_host_to_device_async = &copy_h2d;
        result.memcpy_device_to_host_async = &copy_d2h;
        result.memcpy_device_to_device_async = &copy_d2d;
        result.memset_d8_async = &memset_d8;
        result.launch_kernel = &launch_kernel;
        result.monotonic_time_ns = &now;
        return result;
    }

    static rt::CudaDriverResult push_context(
        void* user_data,
        rt::CudaContext requested) noexcept {
        return user_data && requested == context
            ? rt::CudaDriverResult::success
            : rt::CudaDriverResult::context_lost;
    }

    static rt::CudaDriverResult pop_context(
        void* user_data,
        rt::CudaContext* output) noexcept {
        if (!user_data || !output) {
            return rt::CudaDriverResult::invalid_value;
        }
        *output = context;
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult event_create(
        void* user_data,
        rt::CudaEvent* output) noexcept {
        auto* driver = static_cast<NoAllocCudaDriver*>(user_data);
        if (!driver || !output) {
            return rt::CudaDriverResult::invalid_value;
        }
        *output = driver->next_event++;
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult event_operation(
        void* user_data,
        rt::CudaEvent) noexcept {
        return user_data
            ? rt::CudaDriverResult::success
            : rt::CudaDriverResult::invalid_value;
    }

    static rt::CudaDriverResult event_record(
        void* user_data,
        rt::CudaEvent,
        rt::CudaStream) noexcept {
        return user_data
            ? rt::CudaDriverResult::success
            : rt::CudaDriverResult::invalid_value;
    }

    static rt::CudaDriverResult stream_operation(
        void* user_data,
        rt::CudaStream) noexcept {
        return user_data
            ? rt::CudaDriverResult::success
            : rt::CudaDriverResult::invalid_value;
    }

    static rt::CudaDriverResult mem_alloc(
        void* user_data,
        std::uint64_t,
        rt::CudaDeviceAddress* output) noexcept {
        if (!user_data || !output) {
            return rt::CudaDriverResult::invalid_value;
        }
        *output = 1;
        return rt::CudaDriverResult::success;
    }

    static rt::CudaDriverResult mem_free(
        void* user_data,
        rt::CudaDeviceAddress) noexcept {
        return user_data
            ? rt::CudaDriverResult::success
            : rt::CudaDriverResult::invalid_value;
    }

    static rt::CudaDriverResult host_register(
        void* user_data,
        void*,
        std::uint64_t) noexcept {
        return user_data
            ? rt::CudaDriverResult::success
            : rt::CudaDriverResult::invalid_value;
    }

    static rt::CudaDriverResult host_unregister(
        void* user_data,
        void*) noexcept {
        return user_data
            ? rt::CudaDriverResult::success
            : rt::CudaDriverResult::invalid_value;
    }

    static rt::CudaDriverResult copy_h2d(
        void* user_data,
        rt::CudaDeviceAddress,
        const void*,
        std::uint64_t,
        rt::CudaStream) noexcept {
        return user_data
            ? rt::CudaDriverResult::success
            : rt::CudaDriverResult::invalid_value;
    }

    static rt::CudaDriverResult copy_d2h(
        void* user_data,
        void*,
        rt::CudaDeviceAddress,
        std::uint64_t,
        rt::CudaStream) noexcept {
        return user_data
            ? rt::CudaDriverResult::success
            : rt::CudaDriverResult::invalid_value;
    }

    static rt::CudaDriverResult copy_d2d(
        void* user_data,
        rt::CudaDeviceAddress,
        rt::CudaDeviceAddress,
        std::uint64_t,
        rt::CudaStream) noexcept {
        return user_data
            ? rt::CudaDriverResult::success
            : rt::CudaDriverResult::invalid_value;
    }

    static rt::CudaDriverResult memset_d8(
        void* user_data,
        rt::CudaDeviceAddress,
        std::uint8_t,
        std::uint64_t,
        rt::CudaStream) noexcept {
        return user_data
            ? rt::CudaDriverResult::success
            : rt::CudaDriverResult::invalid_value;
    }

    static rt::CudaDriverResult launch_kernel(
        void* user_data,
        rt::CudaFunction,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        rt::CudaStream,
        void* const*) noexcept {
        return user_data
            ? rt::CudaDriverResult::success
            : rt::CudaDriverResult::invalid_value;
    }

    static std::uint64_t now(void*) noexcept {
        return 1;
    }

    rt::CudaEvent next_event = 1;
};

} // namespace

TEST(TraceNoAlloc, CudaSubmitAndPollDoNotAllocateAfterInitialization) {
    NoAllocCudaDriver driver;
    const std::array<rt::CudaStream, 1> streams{0x51u};
    rt::CudaBackendConfig config{};
    config.queue_capacity = 8;
    config.buffer_capacity = 1;
    config.kernel_capacity = 1;
    config.context = NoAllocCudaDriver::context;
    config.streams = streams;
    rt::CudaDeviceBackend backend(driver.api(), config);
    auto api = backend.api();
    rtfw_device_init_config initialize{};
    initialize.struct_size = sizeof(initialize);
    initialize.abi_version = RTFW_DEVICE_ABI_VERSION;
    initialize.requested_in_flight = 8;
    ASSERT_EQ(
        api.initialize(api.instance, &initialize),
        RTFW_DEVICE_STATUS_OK);

    auto requested = rt::make_device_submission();
    requested.timeout_ns = 1'000;
    requested.opcode = rt::cuda_device_opcode_noop;
    rtfw_device_completion completion{};
    std::uint64_t completion_count = 0;
    {
        AllocationGuard guard;
        for (std::uint64_t index = 1; index <= 64; ++index) {
            requested.submission_id = index;
            ASSERT_EQ(
                api.submit(api.instance, &requested),
                RTFW_DEVICE_STATUS_OK);
            completion_count = 0;
            ASSERT_EQ(
                api.poll(
                    api.instance,
                    &completion,
                    1,
                    &completion_count),
                RTFW_DEVICE_STATUS_OK);
            ASSERT_EQ(completion_count, 1u);
        }
    }
    EXPECT_EQ(allocation_count(), 0u);
    EXPECT_EQ(api.shutdown(api.instance), RTFW_DEVICE_STATUS_OK);
}

TEST(TraceNoAlloc, RatePlanInspectionAndCpuFramesDoNotAllocateCrossRate) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 2;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    std::array<std::uint32_t, 2> execution{};
    std::size_t execution_count = 0;
    RuntimeAllocationProbe first{1, &execution, &execution_count};
    RuntimeAllocationProbe second{2, &execution, &execution_count};
    rt::PhaseHandle first_phase;
    rt::PhaseHandle second_phase;
    ASSERT_EQ(runtime.register_callback({"first", &record_runtime_phase, &first}, first_phase), rt::Status::ok);
    ASSERT_EQ(runtime.register_callback({"second", &record_runtime_phase, &second}, second_phase), rt::Status::ok);
    rt::RateDomainHandle fast;
    rt::RateDomainHandle slow;
    ASSERT_EQ(runtime.register_rate_domain({"fast", 2, 2, 1, 1}, fast), rt::Status::ok);
    ASSERT_EQ(runtime.register_rate_domain({"slow", 3, 1, 1, 0}, slow), rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(first_phase, fast), rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(second_phase, slow), rt::Status::ok);
    const std::array initial{
        std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};
    rt::CrossRateChannelHandle channel;
    ASSERT_EQ(
        runtime.register_cross_rate_channel(
            {"first.to.second", first_phase, second_phase, initial.size(), initial},
            channel),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    bool complete = true;
    {
        AllocationGuard guard;
        for (std::size_t index = 0; index < runtime.rate_domain_count(); ++index) {
            rt::CompiledRateDomain domain;
            complete = runtime.compiled_rate_domain_at(index, domain) && complete;
        }
        for (std::size_t index = 0; index < runtime.rate_binding_count(); ++index) {
            rt::CompiledRateBinding binding;
            complete = runtime.compiled_rate_binding_at(index, binding) && complete;
        }
        for (std::size_t index = 0; index < runtime.reference_release_count(); ++index) {
            rt::ReferenceRelease release;
            complete = runtime.reference_release_at(index, release) && complete;
        }
        for (std::size_t index = 0; index < runtime.cross_rate_channel_count(); ++index) {
            rt::CompiledCrossRateChannel descriptor;
            complete = runtime.compiled_cross_rate_channel_at(index, descriptor) && complete;
            std::array<std::byte, 3> copied{};
            complete = runtime.copy_cross_rate_initial_sample(index, copied) ==
                    rt::Status::ok &&
                copied == initial && complete;
        }
        for (std::size_t index = 0; index < runtime.cross_rate_selection_count(); ++index) {
            rt::CompiledCrossRateSelection selection;
            complete = runtime.compiled_cross_rate_selection_at(index, selection) && complete;
        }
        complete = runtime.step({0, std::chrono::nanoseconds{1}, std::nullopt}) ==
                rt::Status::ok &&
            complete;
    }
    EXPECT_TRUE(complete);
    EXPECT_EQ(allocation_count(), 0u);
    EXPECT_EQ(execution_count, 2u);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
    {
        AllocationGuard guard;
        rt::ReferenceRelease release;
        rt::CompiledCrossRateChannel descriptor;
        complete = runtime.reference_release_at(0, release) &&
            runtime.compiled_cross_rate_channel_at(0, descriptor);
    }
    EXPECT_TRUE(complete);
    EXPECT_EQ(allocation_count(), 0u);
}

namespace {

struct ActiveNoAllocClock final : rt::RuntimeClock {
    std::uint64_t now = 1'000;
    std::uint64_t now_ns() noexcept override { return now; }
};

rt::CallbackResult active_noalloc_callback(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& count = *static_cast<std::size_t*>(user_data);
    if (!context.rate_release) {
        return rt::CallbackResult::error;
    }
    ++count;
    return rt::CallbackResult::ok;
}

} // namespace

TEST(TraceNoAlloc, RateDispatchOnTimeAndLateDegradeDoNotAllocate) {
    ActiveNoAllocClock clock;
    rt::Runtime runtime(clock);
    rt::RuntimeConfig config;
    config.callback_capacity = 1;
    config.executor_queue_capacity = 2;
    config.task_scratch_slots = 2;
    config.watchdog_max_degradation_level = 2;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(runtime.set_rate_execution_policy({2}), rt::Status::ok);
    std::size_t calls = 0;
    rt::PhaseHandle phase;
    rt::RateDomainHandle domain;
    ASSERT_EQ(runtime.register_callback(
                  {"active", &active_noalloc_callback, &calls}, phase),
              rt::Status::ok);
    ASSERT_EQ(runtime.register_rate_domain(
                  {"rate", 100, 1, 100, 10,
                   rt::RateCriticality::normal, false,
                   rt::RateLateAction::degrade, 0},
                  domain),
              rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(phase, domain), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    rt::StepResult first;
    rt::StepResult second;
    bool complete = false;
    {
        AllocationGuard guard;
        complete = runtime.step(
                       {0, std::chrono::nanoseconds{100},
                        std::nullopt, 1'000},
                       &first) == rt::Status::ok;
        clock.now = 10'000;
        complete = runtime.step(
                       {1, std::chrono::nanoseconds{100},
                        std::nullopt, 1'100},
                       &second) == rt::Status::ok &&
            complete;
    }
    EXPECT_TRUE(complete);
    EXPECT_EQ(allocation_count(), 0u);
    EXPECT_EQ(calls, 2u);
    EXPECT_EQ(first.rate.on_time_domain_releases, 1u);
    EXPECT_EQ(second.rate.degraded_domain_releases, 1u);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}
