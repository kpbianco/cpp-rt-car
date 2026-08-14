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

extern "C" rtfw_status RTFW_EXTENSION_CALL rtfw_extension_entry_v1(
    const rtfw_extension_host_api_v1*, rtfw_extension_descriptor_v1*);

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace {
std::atomic<std::size_t> g_allocation_count{0};
std::atomic<bool> g_track_allocations{false};
std::atomic<std::ptrdiff_t> g_fail_after{-1};

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
    const auto remaining = g_fail_after.load(std::memory_order_relaxed);
    if (remaining >= 0 &&
        g_fail_after.fetch_sub(1, std::memory_order_relaxed) == 0) {
        throw std::bad_alloc();
    }
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
    const auto remaining = g_fail_after.load(std::memory_order_relaxed);
    if (remaining >= 0 &&
        g_fail_after.fetch_sub(1, std::memory_order_relaxed) == 0) {
        throw std::bad_alloc();
    }
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

struct NoAllocNativeHalV2Backend {
    rt::HalV2BackendApi api() noexcept {
        rt::HalV2BackendApi table;
        table.instance = this;
        table.get_capabilities = &get_capabilities;
        table.initialize = &initialize;
        table.register_buffer = &register_buffer;
        table.unregister_buffer = &unregister_buffer;
        table.submit = &submit;
        table.poll = &poll;
        table.cancel = &cancel;
        table.get_health = &get_health;
        table.reset = &reset;
        table.shutdown = &shutdown;
        return table;
    }

    static NoAllocNativeHalV2Backend* self(void* instance) noexcept {
        return static_cast<NoAllocNativeHalV2Backend*>(instance);
    }

    static rt::HalV2Status get_capabilities(
        void* instance,
        rt::HalV2Capabilities* output) noexcept {
        if (!self(instance) || !output ||
            output->struct_size < sizeof(*output)) {
            return rt::HalV2Status::invalid_argument;
        }
        *output = {};
        output->struct_size = sizeof(*output);
        output->api_version = rt::hal_v2_api_version;
        output->max_in_flight = 4;
        output->max_registered_buffers = 1;
        output->max_buffer_bytes = 4096;
        output->inline_payload_capacity =
            rt::hal_v2_inline_payload_capacity;
        output->buffer_ref_capacity =
            rt::hal_v2_buffer_ref_capacity;
        output->supports_cancel = 1;
        output->supports_reset = 1;
        output->deterministic_mock = 1;
        constexpr std::array identifier{
            't', 'e', 's', 't', '.', 'n', 'a', 't', 'i', 'v', 'e',
            '.', 'h', 'a', 'l', '.', 'v', '2'};
        std::copy(
            identifier.begin(),
            identifier.end(),
            output->backend_id.begin());
        output->control_storage_bytes = sizeof(NoAllocNativeHalV2Backend);
        return rt::HalV2Status::ok;
    }

    static rt::HalV2Status initialize(
        void* instance,
        const rt::HalV2InitializeConfig* config) noexcept {
        auto* backend = self(instance);
        if (!backend || !config || config->struct_size < sizeof(*config) ||
            config->api_version != rt::hal_v2_api_version ||
            config->requested_in_flight == 0 ||
            config->requested_in_flight > 4 ||
            config->requested_registered_buffers > 1 ||
            !std::all_of(config->reserved.begin(), config->reserved.end(),
                         [](std::uint64_t value) { return value == 0; })) {
          return rt::HalV2Status::invalid_argument;
        }
        bool expected = false;
        if (!backend->initialized.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            return rt::HalV2Status::invalid_state;
        }
        return rt::HalV2Status::ok;
    }

    static rt::HalV2Status register_buffer(
        void*,
        const rt::HalV2BufferRegistration*,
        std::uint64_t*) noexcept {
        return rt::HalV2Status::unsupported;
    }

    static rt::HalV2Status unregister_buffer(
        void*, std::uint64_t) noexcept {
        return rt::HalV2Status::unsupported;
    }

    static rt::HalV2Status submit(
        void* instance,
        const rt::HalV2Submission* submission) noexcept {
        auto* backend = self(instance);
        if (!backend || !submission ||
            !backend->initialized.load(std::memory_order_acquire) ||
            submission->struct_size < sizeof(*submission) ||
            submission->api_version != rt::hal_v2_api_version ||
            submission->submission_id == 0 ||
            submission->timeout_ns == 0 ||
            submission->flags != 0 ||
            submission->payload_size != 0 ||
            submission->buffer_count != 0 ||
            !std::all_of(
                submission->reserved.begin(),
                submission->reserved.end(),
                [](std::uint64_t value) { return value == 0; })) {
            return rt::HalV2Status::invalid_argument;
        }
        std::uint64_t expected = 0;
        if (!backend->pending_submission.compare_exchange_strong(
                expected,
                submission->submission_id,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            backend->queue_rejections.fetch_add(
                1, std::memory_order_relaxed);
            return rt::HalV2Status::queue_full;
        }
        backend->submissions.fetch_add(1, std::memory_order_relaxed);
        return rt::HalV2Status::ok;
    }

    static rt::HalV2Status poll(
        void* instance,
        rt::HalV2Completion* output,
        std::uint64_t output_capacity,
        std::uint64_t* output_count) noexcept {
        auto* backend = self(instance);
        if (!backend || !output_count ||
            !backend->initialized.load(std::memory_order_acquire) ||
            (output_capacity != 0 && !output)) {
            return rt::HalV2Status::invalid_argument;
        }
        *output_count = 0;
        if (output_capacity == 0) {
            return rt::HalV2Status::ok;
        }
        const auto submission_id = backend->pending_submission.exchange(
            0, std::memory_order_acq_rel);
        if (submission_id == 0) {
            return rt::HalV2Status::ok;
        }
        output[0] = {};
        output[0].struct_size = sizeof(output[0]);
        output[0].status =
            static_cast<std::int32_t>(rt::HalV2Status::ok);
        output[0].submission_id = submission_id;
        output[0].device_timestamp_ns = submission_id;
        output[0].value = submission_id;
        *output_count = 1;
        backend->completions.fetch_add(1, std::memory_order_relaxed);
        return rt::HalV2Status::ok;
    }

    static rt::HalV2Status cancel(
        void* instance, std::uint64_t submission_id) noexcept {
        auto* backend = self(instance);
        if (!backend || submission_id == 0 ||
            !backend->initialized.load(std::memory_order_acquire)) {
            return rt::HalV2Status::invalid_argument;
        }
        auto expected = submission_id;
        return backend->pending_submission.compare_exchange_strong(
                   expected,
                   0,
                   std::memory_order_acq_rel,
                   std::memory_order_relaxed)
            ? rt::HalV2Status::ok
            : rt::HalV2Status::invalid_argument;
    }

    static rt::HalV2Status get_health(
        void* instance,
        rt::HalV2Health* output) noexcept {
        auto* backend = self(instance);
        if (!backend || !output ||
            output->struct_size < sizeof(*output)) {
            return rt::HalV2Status::invalid_argument;
        }
        *output = {};
        output->struct_size = sizeof(*output);
        output->state = static_cast<std::uint32_t>(
            backend->initialized.load(std::memory_order_acquire)
                ? rt::HalV2HealthState::healthy
                : rt::HalV2HealthState::shutdown);
        output->last_status =
            static_cast<std::int32_t>(rt::HalV2Status::ok);
        output->submissions =
            backend->submissions.load(std::memory_order_acquire);
        output->completions =
            backend->completions.load(std::memory_order_acquire);
        output->queue_rejections =
            backend->queue_rejections.load(std::memory_order_acquire);
        output->outstanding =
            backend->pending_submission.load(std::memory_order_acquire) == 0
                ? 0
                : 1;
        return rt::HalV2Status::ok;
    }

    static rt::HalV2Status reset(void* instance) noexcept {
        auto* backend = self(instance);
        if (!backend ||
            !backend->initialized.load(std::memory_order_acquire)) {
            return rt::HalV2Status::invalid_state;
        }
        return backend->pending_submission.load(
                   std::memory_order_acquire) == 0
            ? rt::HalV2Status::ok
            : rt::HalV2Status::invalid_state;
    }

    static rt::HalV2Status shutdown(void* instance) noexcept {
        auto* backend = self(instance);
        if (!backend ||
            backend->pending_submission.load(std::memory_order_acquire) != 0) {
            return rt::HalV2Status::invalid_state;
        }
        bool expected = true;
        return backend->initialized.compare_exchange_strong(
                   expected,
                   false,
                   std::memory_order_acq_rel,
                   std::memory_order_relaxed)
            ? rt::HalV2Status::ok
            : rt::HalV2Status::invalid_state;
    }

    std::atomic<bool> initialized{false};
    std::atomic<std::uint64_t> pending_submission{0};
    std::atomic<std::uint64_t> submissions{0};
    std::atomic<std::uint64_t> completions{0};
    std::atomic<std::uint64_t> queue_rejections{0};
};

struct NoAllocHeterogeneousExtension {
  rt::HalV2MemoryTopologyExtension api() noexcept {
    rt::HalV2MemoryTopologyExtension table;
    table.instance = this;
    table.discover = &discover;
    table.register_memory = &register_memory;
    table.unregister_memory = &unregister_memory;
    table.query_timestamp_correlation = &query_correlation;
    return table;
  }

  static NoAllocHeterogeneousExtension *self(void *instance) noexcept {
    return static_cast<NoAllocHeterogeneousExtension *>(instance);
  }

  static rt::HalV2Status
  discover(void *instance, rt::HalV2MemoryTopologySnapshot *output) noexcept {
    if (!self(instance) || !output || output->struct_size < sizeof(*output)) {
      return rt::HalV2Status::invalid_argument;
    }
    *output = {};
    output->memory_domain_count = 1;
    output->topology_node_count = 1;
    output->timestamp_domain_count = 1;
    output->completion_timestamp_domain_identity = 1;
    auto &memory = output->memory_domains[0];
    memory.identity = 1;
    memory.kind = static_cast<std::uint32_t>(rt::HalV2MemoryDomainKind::host);
    memory.ownership_modes = rt::hal_v2_memory_ownership_borrowed_host;
    memory.maximum_bytes = 4096;
    memory.byte_granularity = 1;
    memory.alignment = 64;
    memory.offset_granularity = 1;
    memory.access =
        RTFW_DEVICE_BUFFER_HOST_READ | RTFW_DEVICE_BUFFER_HOST_WRITE |
        RTFW_DEVICE_BUFFER_DEVICE_READ | RTFW_DEVICE_BUFFER_DEVICE_WRITE;
    memory.coherency =
        static_cast<std::uint32_t>(rt::HalV2MemoryCoherency::host_coherent);
    memory.topology_node_identity = 1;
    memory.timestamp_domain_identity = 1;
    auto &node = output->topology_nodes[0];
    node.identity = 1;
    node.kind = static_cast<std::uint32_t>(rt::HalV2TopologyNodeKind::host);
    auto &timestamp = output->timestamp_domains[0];
    timestamp.identity = 1;
    timestamp.kind = static_cast<std::uint32_t>(
        rt::HalV2TimestampDomainKind::backend_device);
    timestamp.tick_numerator_ns = 1;
    timestamp.tick_denominator = 1;
    timestamp.monotonic = 1;
    timestamp.resets_on_backend_reset = 1;
    return rt::HalV2Status::ok;
  }

  static rt::HalV2Status
  register_memory(void *instance,
                  const rt::HalV2MemoryRegistration *registration,
                  rt::HalV2MemoryToken *token) noexcept {
    auto *extension = self(instance);
    if (!extension || !registration || !token ||
        registration->struct_size < sizeof(*registration) ||
        registration->domain_identity != 1 || registration->bytes != 64 ||
        !registration->host_data) {
      return rt::HalV2Status::invalid_argument;
    }
    bool expected = false;
    if (!extension->registered.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      return rt::HalV2Status::invalid_state;
    }
    *token = {};
    token->submission_token = 41;
    return rt::HalV2Status::ok;
  }

  static rt::HalV2Status
  unregister_memory(void *instance, const rt::HalV2MemoryRegistration *,
                    const rt::HalV2MemoryToken *token) noexcept {
    auto *extension = self(instance);
    if (!extension || !token || token->submission_token != 41) {
      return rt::HalV2Status::invalid_argument;
    }
    bool expected = true;
    return extension->registered.compare_exchange_strong(
               expected, false, std::memory_order_acq_rel)
               ? rt::HalV2Status::ok
               : rt::HalV2Status::invalid_state;
  }

  static rt::HalV2Status
  query_correlation(void *, const rt::HalV2TimestampCorrelationQuery *,
                    rt::HalV2TimestampCorrelation *) noexcept {
    return rt::HalV2Status::unsupported;
  }

  std::atomic<bool> registered{false};
};

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

TEST(TraceNoAlloc, NativeHalV2FramesAndStopDoNotAllocateAfterStart) {
    NoAllocNativeHalV2Backend backend;
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 1;
    config.trace_capacity = 256;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    config.device_backend_capacity = 1;
    config.device_buffer_capacity = 1;
    config.device_outstanding_capacity = 4;
    config.device_completion_batch = 4;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);

    rt::DeviceBackendHandle backend_handle;
    ASSERT_EQ(
        runtime.register_device_backend(
            rt::HalV2BackendRegistration{
                "native.noalloc", backend.api()},
            backend_handle),
        rt::Status::ok);
    rt::PhaseHandle phase;
    ASSERT_EQ(
        runtime.register_device_phase(
            {
                "native.noalloc.phase",
                backend_handle,
                &submit_noalloc_device_command,
                nullptr,
            },
            phase),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    bool complete = true;
    {
        AllocationGuard guard;
        for (std::uint64_t frame = 0; complete && frame < 64; ++frame) {
            complete = runtime.step({
                           frame,
                           std::chrono::nanoseconds(1),
                           std::nullopt,
                       }) == rt::Status::ok;
        }
        complete = runtime.stop() == rt::Status::ok && complete;
    }

    EXPECT_TRUE(complete);
    EXPECT_EQ(allocation_count(), 0u);
    EXPECT_FALSE(backend.initialized.load(std::memory_order_acquire));
    EXPECT_EQ(backend.submissions.load(std::memory_order_acquire), 64u);
    EXPECT_EQ(backend.completions.load(std::memory_order_acquire), 64u);
    EXPECT_EQ(
        backend.queue_rejections.load(std::memory_order_acquire),
        0u);
}

TEST(TraceNoAlloc, HeterogeneousInspectorsHealthAndStopDoNotAllocate) {
  NoAllocNativeHalV2Backend backend;
  NoAllocHeterogeneousExtension extension;
  auto extension_api = extension.api();
  rt::Runtime runtime;
  rt::RuntimeConfig config;
  config.trace_capacity = 64;
  config.executor_queue_capacity = 8;
  config.task_scratch_slots = 8;
  config.device_backend_capacity = 1;
  config.device_buffer_capacity = 1;
  config.device_outstanding_capacity = 4;
  config.device_completion_batch = 4;
  ASSERT_EQ(runtime.configure(config), rt::Status::ok);
  rt::DeviceBackendHandle backend_handle;
  ASSERT_EQ(runtime.register_device_backend(
                {"native.heterogeneous.noalloc", backend.api(), &extension_api},
                backend_handle),
            rt::Status::ok);
  rt::DeviceMemoryDomainHandle domain;
  rt::HalV2MemoryDomain descriptor;
  ASSERT_TRUE(
      runtime.device_memory_domain_at(backend_handle, 0, domain, descriptor));
  alignas(64) std::array<std::byte, 64> storage{};
  rt::DeviceBufferHandle buffer;
  ASSERT_EQ(runtime.register_device_buffer(
                {
                    "native.heterogeneous.noalloc.buffer",
                    backend_handle,
                    domain,
                    storage,
                    {},
                    storage.size(),
                    rt::HalV2MemoryOwnership::borrowed_host,
                    descriptor.access,
                    rt::HalV2MemoryCoherency::host_coherent,
                    rt::hal_v2_memory_sync_none,
                },
                buffer),
            rt::Status::ok);
  ASSERT_EQ(runtime.finalize(), rt::Status::ok);
  ASSERT_EQ(runtime.start(), rt::Status::ok);

  bool complete = true;
  {
    AllocationGuard guard;
    for (std::size_t index = 0; index < 64; ++index) {
      rt::DeviceMemoryDomainHandle inspected_domain;
      rt::HalV2MemoryDomain inspected_descriptor;
      rt::DeviceMemoryObjectInfo object;
      rt::DeviceHealth health;
      complete = complete &&
                 runtime.device_memory_domain_at(
                     backend_handle, 0, inspected_domain, inspected_descriptor);
      complete = complete && runtime.device_memory_object_at(0, object);
      complete = complete && runtime.device_health(backend_handle, health) ==
                                 rt::Status::ok;
    }
    complete = runtime.stop() == rt::Status::ok && complete;
  }
  EXPECT_TRUE(complete);
  EXPECT_EQ(allocation_count(), 0u);
  EXPECT_FALSE(extension.registered.load(std::memory_order_acquire));
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

TEST(TraceNoAlloc, RateTelemetryShedRecoverInspectAndStopDoNotAllocate) {
    ActiveNoAllocClock clock;
    clock.now = 10'000;
    rt::Runtime runtime(clock);
    rt::RuntimeConfig config;
    config.callback_capacity = 2;
    config.executor_queue_capacity = 2;
    config.task_scratch_slots = 2;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(
        runtime.set_rate_execution_policy({8, 3, 1, 1, 8}),
        rt::Status::ok);
    std::array<std::size_t, 2> calls{};
    std::array<rt::PhaseHandle, 2> phases{};
    std::array<rt::RateDomainHandle, 2> domains{};
    ASSERT_EQ(runtime.register_callback(
                  {"mandatory", &active_noalloc_callback, &calls[0]}, phases[0]),
              rt::Status::ok);
    ASSERT_EQ(runtime.register_callback(
                  {"optional", &active_noalloc_callback, &calls[1]}, phases[1]),
              rt::Status::ok);
    ASSERT_EQ(runtime.register_rate_domain(
                  {"mandatory", 100, 1, 50, 10,
                   rt::RateCriticality::critical, false,
                   rt::RateLateAction::skip, 0}, domains[0]),
              rt::Status::ok);
    ASSERT_EQ(runtime.register_rate_domain(
                  {"optional", 100, 1, 50, 10,
                   rt::RateCriticality::background, true,
                   rt::RateLateAction::fail, 0}, domains[1]),
              rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(phases[0], domains[0]), rt::Status::ok);
    ASSERT_EQ(runtime.bind_phase_to_rate_domain(phases[1], domains[1]), rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    rt::StepResult late;
    rt::StepResult recovered;
    rt::RateTelemetryMetadata metadata;
    rt::RateCounterSnapshot counters;
    rt::RateTelemetryCursor cursor;
    rt::RateTelemetryReadResult read;
    std::array<rt::RateActionRecord, 8> records{};
    bool complete = false;
    {
        AllocationGuard guard;
        complete = runtime.step(
                       {0, std::chrono::nanoseconds{100}, std::nullopt, 1'000},
                       &late) == rt::Status::ok;
        clock.now = 1'100;
        complete = runtime.step(
                       {1, std::chrono::nanoseconds{100}, std::nullopt, 1'100},
                       &recovered) == rt::Status::ok && complete;
        complete = runtime.rate_telemetry_metadata(metadata) == rt::Status::ok &&
            runtime.rate_counters_snapshot(counters) == rt::Status::ok &&
            runtime.read_rate_actions(cursor, records, read) == rt::Status::ok &&
            runtime.stop() == rt::Status::ok && complete;
    }
    EXPECT_TRUE(complete);
    EXPECT_EQ(allocation_count(), 0u);
    EXPECT_EQ(late.rate.shed_transitions, 1u);
    EXPECT_EQ(recovered.rate.recovery_transitions, 1u);
    EXPECT_EQ(read.records_read, 4u);
}

TEST(TraceNoAlloc, ExtensionExecuteInspectStopAndDetachDoNotAllocate) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 2;
    config.executor_queue_capacity = 2;
    config.task_scratch_slots = 2;
    config.trace_capacity = 16;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    rt::ExtensionHandle extension;
    ASSERT_EQ(
        runtime.register_extension(&rtfw_extension_entry_v1, extension),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    rt::ExtensionInfo info;
    bool ready = false;
    bool complete = false;
    {
        AllocationGuard guard;
        complete = runtime.step({
            1, std::chrono::milliseconds(1), std::nullopt}) == rt::Status::ok;
        complete = runtime.extension_info(extension, info) == rt::Status::ok &&
            runtime.stop() == rt::Status::ok && complete;
        complete = runtime.detach_extension(extension, ready) == rt::Status::ok &&
            complete;
    }
    EXPECT_TRUE(complete);
    EXPECT_TRUE(ready);
    EXPECT_EQ(allocation_count(), 0u);
}

TEST(ExtensionRegistration, AllocationFailurePublishesNothingAndRecovers) {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 2;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    rt::ExtensionHandle extension;
    g_fail_after.store(0, std::memory_order_release);
    const auto failure =
        runtime.register_extension(&rtfw_extension_entry_v1, extension);
    g_fail_after.store(-1, std::memory_order_release);
    EXPECT_EQ(failure, rt::Status::resource_exhausted);
    EXPECT_FALSE(extension.valid());
    EXPECT_EQ(runtime.extension_count(), 0u);
    EXPECT_EQ(runtime.callback_count(), 0u);
    EXPECT_EQ(
        runtime.register_extension(&rtfw_extension_entry_v1, extension),
        rt::Status::ok);
}
