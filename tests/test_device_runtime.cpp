#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include <rt/mock_device.hpp>
#include <rt/runtime.hpp>

#include "rt/src/thread_policy.hpp"

#if defined(__SANITIZE_ADDRESS__)
#define RTFW_DEVICE_TEST_ADDRESS_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define RTFW_DEVICE_TEST_ADDRESS_SANITIZER 1
#endif
#endif

#if defined(__SANITIZE_THREAD__)
#define RTFW_DEVICE_TEST_THREAD_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define RTFW_DEVICE_TEST_THREAD_SANITIZER 1
#endif
#endif

namespace {

using namespace std::chrono_literals;

rt::RuntimeConfig device_config(
    std::size_t callback_capacity = 4,
    std::size_t worker_count = 2,
    std::size_t outstanding_capacity = 4) {
    rt::RuntimeConfig config;
    config.callback_capacity = callback_capacity;
    config.worker_count = worker_count;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 16;
    config.trace_capacity = 64;
    config.device_backend_capacity = 2;
    config.device_buffer_capacity = 8;
    config.device_outstanding_capacity = outstanding_capacity;
    config.device_completion_batch =
        std::min<std::size_t>(outstanding_capacity, 4);
    return config;
}

struct CleanupMemoryProvider {
    static constexpr std::size_t slot_bytes = 128 * 1024;
    struct alignas(64) Slot {
        std::array<std::byte, slot_bytes> bytes{};
        rt::MemoryRegionId region{};
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
    std::vector<std::uint32_t>* calls = nullptr;
    std::size_t rollback_count = 0;
    std::size_t release_count = 0;

    static constexpr std::uint32_t rollback_event = 500;
    static constexpr std::uint32_t release_event = 600;

    static rt::Status acquire(
        void* user_data,
        const rt::MemoryProviderAcquireRequest& request,
        rt::MemoryProviderAllocation& allocation) noexcept {
        auto& self = *static_cast<CleanupMemoryProvider*>(user_data);
        const auto index = static_cast<std::size_t>(
            request.region.value - rt::memory_region_phase_scratch.value);
        if (index >= self.slots.size() ||
            request.logical_bytes > self.slots[index].bytes.size() ||
            request.required_alignment > 64) {
            return rt::Status::resource_exhausted;
        }
        auto& slot = self.slots[index];
        slot.region = request.region;
        allocation.token = &slot;
        allocation.allocation_base = slot.bytes.data();
        allocation.allocation_bytes = request.logical_bytes;
        allocation.usable_data = slot.bytes.data();
        allocation.usable_bytes = request.logical_bytes;
        allocation.committed_bytes = request.logical_bytes;
        allocation.alignment = 64;
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
        void* user_data,
        void* token,
        const rt::MemoryPolicy&,
        const rt::MemoryProviderObservation&) noexcept {
        auto& self = *static_cast<CleanupMemoryProvider*>(user_data);
        const auto& slot = *static_cast<Slot*>(token);
        ++self.rollback_count;
        if (self.calls) {
            self.calls->push_back(rollback_event + slot.region.value);
        }
        return rt::Status::ok;
    }

    static void release(
        void* user_data,
        void* token,
        rt::RollbackIntent) noexcept {
        auto& self = *static_cast<CleanupMemoryProvider*>(user_data);
        const auto& slot = *static_cast<Slot*>(token);
        ++self.release_count;
        if (self.calls) {
            self.calls->push_back(release_event + slot.region.value);
        }
    }
};

class FailingDeviceServiceThreadProvider final
    : public rt::detail::ThreadPolicyProvider {
public:
    [[nodiscard]] rt::Status resolve(
        rt::ThreadRoleId,
        rt::PolicyApplicationMode,
        bool active,
        bool,
        bool,
        const rt::ThreadPolicy& requested,
        const rt::ThreadPolicy& role_default,
        rt::ThreadPolicy& resolved,
        rt::PolicyResolutionState& resolution,
        std::int32_t& system_error) noexcept override {
        resolved = role_default;
        resolved.requirement = requested.requirement;
        resolution = active
            ? rt::PolicyResolutionState::native_supported
            : rt::PolicyResolutionState::inactive;
        system_error = 0;
        return rt::Status::ok;
    }

    [[nodiscard]] rt::Status before_create(
        rt::ThreadRoleId,
        std::size_t,
        const rt::detail::ThreadRolePlan&,
        std::int32_t& system_error) noexcept override {
        system_error = 0;
        return rt::Status::ok;
    }

    void apply_and_verify_current(
        rt::ThreadRoleId role,
        std::size_t,
        const rt::detail::ThreadRolePlan& plan,
        rt::detail::ThreadStartupResult& result) noexcept override {
        result.read_back = plan.resolved;
        result.applied = rt::PolicyOperationState::succeeded;
        result.verified = rt::PolicyOperationState::succeeded;
        if (role == rt::thread_role_device_service && fail) {
            result.verified = rt::PolicyOperationState::mismatched;
            result.verify_error = 91;
        }
    }

    void verify_current(
        rt::ThreadRoleId,
        const rt::detail::ThreadRolePlan& plan,
        rt::detail::ThreadStartupResult& result) noexcept override {
        result.read_back = plan.resolved;
        result.verified = rt::PolicyOperationState::succeeded;
    }

    [[nodiscard]] rt::Status cleanup_stack_current(
        rt::ThreadRoleId role,
        std::size_t instance,
        const rt::detail::ThreadRolePlan& plan,
        rt::detail::ThreadStartupResult& result) noexcept override {
        if (role == rt::thread_role_device_service) {
            ++device_cleanup_count;
            device_cleanup_thread = std::this_thread::get_id();
            if (device_cleanup_failures_remaining != 0) {
                --device_cleanup_failures_remaining;
                result.stack_cleanup = rt::PolicyOperationState::failed;
                result.stack_cleanup_error = EIO;
                return rt::Status::internal_error;
            }
        }
        return ThreadPolicyProvider::cleanup_stack_current(
            role,
            instance,
            plan,
            result);
    }

    void after_join(rt::ThreadRoleId role, std::size_t) noexcept override {
        if (role == rt::thread_role_device_service) {
            rollback_count_seen_at_join = memory_provider
                ? memory_provider->rollback_count
                : std::numeric_limits<std::size_t>::max();
            ++device_join_count;
        }
    }

    CleanupMemoryProvider* memory_provider = nullptr;
    bool fail = true;
    std::size_t device_cleanup_failures_remaining = 0;
    std::size_t device_cleanup_count = 0;
    std::size_t device_join_count = 0;
    std::thread::id device_cleanup_thread{};
    std::size_t rollback_count_seen_at_join =
        std::numeric_limits<std::size_t>::max();
};

rt::CallbackResult submit_noop(
    void*,
    const rt::DeviceCallbackContext&,
    rt::DeviceSubmission& submission) {
    submission.timeout_ns = 10'000;
    submission.opcode = rt::mock_device_opcode_noop;
    return rt::CallbackResult::ok;
}

rtfw_device_status report_nondeterministic_capabilities(
    void*,
    rtfw_device_capabilities* capabilities) noexcept {
    if (!capabilities ||
        capabilities->struct_size < sizeof(*capabilities)) {
        return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
    }
    *capabilities = {};
    capabilities->struct_size = sizeof(*capabilities);
    capabilities->abi_version = RTFW_DEVICE_ABI_VERSION;
    capabilities->max_in_flight = 64;
    capabilities->max_registered_buffers = 64;
    capabilities->max_buffer_bytes =
        std::numeric_limits<std::uint64_t>::max();
    capabilities->inline_payload_capacity =
        RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY;
    capabilities->buffer_ref_capacity =
        RTFW_DEVICE_BUFFER_REF_CAPACITY;
    capabilities->supports_cancel = 1;
    capabilities->supports_reset = 1;
    constexpr std::string_view id = "test.nondeterministic";
    std::copy(
        id.begin(),
        id.end(),
        capabilities->backend_id);
    return RTFW_DEVICE_STATUS_OK;
}

struct FillCommand {
    rt::DeviceBufferHandle buffer{};
    std::size_t bytes = 0;
    std::uint8_t value = 0;
    std::atomic<std::uint32_t>* sequence = nullptr;
    std::uint32_t order = 0;
};

rt::CallbackResult submit_fill(
    void* user_data,
    const rt::DeviceCallbackContext&,
    rt::DeviceSubmission& submission) {
    auto& command = *static_cast<FillCommand*>(user_data);
    if (command.sequence) {
        command.order =
            command.sequence->fetch_add(
                1,
                std::memory_order_acq_rel) + 1;
    }
    submission.timeout_ns = 10'000'000;
    submission.opcode = rt::mock_device_opcode_fill;
    submission.payload_size = 1;
    submission.payload[0] = command.value;
    submission.buffer_count = 1;
    submission.buffers[0].buffer_token = command.buffer.value;
    submission.buffers[0].access = RTFW_DEVICE_ACCESS_WRITE;
    submission.buffers[0].bytes = command.bytes;
    return rt::CallbackResult::ok;
}

struct CpuOrderProbe {
    std::atomic<std::uint32_t>* sequence = nullptr;
    std::span<const std::byte> expected{};
    std::byte expected_value{};
    std::uint32_t order = 0;
    bool contents_match = false;
};

rt::CallbackResult record_cpu_order(
    void* user_data,
    const rt::CallbackContext&) {
    auto& probe = *static_cast<CpuOrderProbe*>(user_data);
    probe.contents_match =
        probe.expected.empty() ||
        std::all_of(
            probe.expected.begin(),
            probe.expected.end(),
            [&](std::byte value) {
                return value == probe.expected_value;
            });
    probe.order =
        probe.sequence->fetch_add(
            1,
            std::memory_order_acq_rel) + 1;
    return rt::CallbackResult::ok;
}

rt::CallbackResult no_op_cpu(
    void*,
    const rt::CallbackContext&) {
    return rt::CallbackResult::ok;
}

struct TeardownProbe {
    std::uint32_t id = 0;
    std::vector<std::uint32_t>* calls = nullptr;
    rtfw_device_status initialize_failure = RTFW_DEVICE_STATUS_OK;
    rtfw_device_status register_failure = RTFW_DEVICE_STATUS_OK;
    rtfw_device_status unregister_failure = RTFW_DEVICE_STATUS_OK;
    rtfw_device_status shutdown_failure = RTFW_DEVICE_STATUS_OK;
    std::size_t initialize_failures_remaining = 0;
    std::size_t register_failure_call = 0;
    std::size_t unregister_failures_remaining = 0;
    std::size_t shutdown_failures_remaining = 0;
    std::size_t initialize_calls = 0;
    std::size_t register_calls = 0;
    std::size_t unregister_calls = 0;
    std::size_t shutdown_calls = 0;
    std::size_t shutdown_with_registered_buffers = 0;
    std::size_t registered_buffers = 0;
    std::uint64_t next_buffer_token = 1;
    bool initialize_failure_retains_ownership = false;
    bool initialized = false;

    static constexpr std::uint32_t initialize_event = 100;
    static constexpr std::uint32_t register_event = 200;
    static constexpr std::uint32_t unregister_event = 300;
    static constexpr std::uint32_t shutdown_event = 400;

    void record(std::uint32_t event) {
        if (calls) {
            calls->push_back(event + id);
        }
    }

    static TeardownProbe* self(void* instance) {
        return static_cast<TeardownProbe*>(instance);
    }

    static rtfw_device_status get_capabilities(
        void* instance,
        rtfw_device_capabilities* capabilities) noexcept {
        auto* probe = self(instance);
        if (!probe || !capabilities ||
            capabilities->struct_size < sizeof(*capabilities)) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        *capabilities = {};
        capabilities->struct_size = sizeof(*capabilities);
        capabilities->abi_version = RTFW_DEVICE_ABI_VERSION;
        capabilities->max_in_flight = 8;
        capabilities->max_registered_buffers = 8;
        capabilities->max_buffer_bytes =
            std::numeric_limits<std::uint64_t>::max();
        capabilities->inline_payload_capacity =
            RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY;
        capabilities->buffer_ref_capacity =
            RTFW_DEVICE_BUFFER_REF_CAPACITY;
        capabilities->supports_cancel = 1;
        capabilities->supports_reset = 1;
        capabilities->deterministic_mock = 1;
        const auto name =
            probe->id == 0
            ? std::string_view{"test.teardown.0"}
            : std::string_view{"test.teardown.1"};
        std::copy(
            name.begin(),
            name.end(),
            capabilities->backend_id);
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status initialize(
        void* instance,
        const rtfw_device_init_config*) noexcept {
        auto* probe = self(instance);
        if (!probe) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        ++probe->initialize_calls;
        probe->record(initialize_event);
        if (probe->initialize_failures_remaining != 0) {
            --probe->initialize_failures_remaining;
            probe->initialized =
                probe->initialize_failure_retains_ownership;
            return probe->initialize_failure;
        }
        probe->initialized = true;
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status register_buffer(
        void* instance,
        const rtfw_device_buffer_registration*,
        std::uint64_t* out_token) noexcept {
        auto* probe = self(instance);
        if (!probe || !out_token || !probe->initialized) {
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }
        ++probe->register_calls;
        probe->record(register_event);
        *out_token = 0;
        if (probe->register_calls == probe->register_failure_call) {
            return probe->register_failure;
        }
        *out_token =
            (static_cast<std::uint64_t>(probe->id + 1) << 32u) |
            probe->next_buffer_token++;
        ++probe->registered_buffers;
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status unregister_buffer(
        void* instance,
        std::uint64_t) noexcept {
        auto* probe = self(instance);
        if (!probe || !probe->initialized ||
            probe->registered_buffers == 0) {
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }
        ++probe->unregister_calls;
        probe->record(unregister_event);
        if (probe->unregister_failures_remaining != 0) {
            --probe->unregister_failures_remaining;
            return probe->unregister_failure;
        }
        --probe->registered_buffers;
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status submit(
        void*,
        const rtfw_device_submission*) noexcept {
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status poll(
        void*,
        rtfw_device_completion*,
        std::uint64_t,
        std::uint64_t* out_count) noexcept {
        if (!out_count) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        *out_count = 0;
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status cancel(
        void*,
        std::uint64_t) noexcept {
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status get_health(
        void* instance,
        rtfw_device_health* health) noexcept {
        auto* probe = self(instance);
        if (!probe || !health ||
            health->struct_size < sizeof(*health)) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        *health = {};
        health->struct_size = sizeof(*health);
        health->state = probe->initialized
            ? RTFW_DEVICE_HEALTH_HEALTHY
            : RTFW_DEVICE_HEALTH_SHUTDOWN;
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status reset(void*) noexcept {
        return RTFW_DEVICE_STATUS_OK;
    }

    static rtfw_device_status shutdown(void* instance) noexcept {
        auto* probe = self(instance);
        if (!probe) {
            return RTFW_DEVICE_STATUS_INVALID_ARGUMENT;
        }
        ++probe->shutdown_calls;
        probe->record(shutdown_event);
        if (!probe->initialized) {
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }
        if (probe->registered_buffers != 0) {
            ++probe->shutdown_with_registered_buffers;
            return RTFW_DEVICE_STATUS_INVALID_STATE;
        }
        if (probe->shutdown_failures_remaining != 0) {
            --probe->shutdown_failures_remaining;
            return probe->shutdown_failure;
        }
        probe->initialized = false;
        return RTFW_DEVICE_STATUS_OK;
    }

    rtfw_device_backend_api api() {
        rtfw_device_backend_api output{};
        output.struct_size = sizeof(output);
        output.abi_version = RTFW_DEVICE_ABI_VERSION;
        output.instance = this;
        output.get_capabilities = &get_capabilities;
        output.initialize = &initialize;
        output.register_buffer = &register_buffer;
        output.unregister_buffer = &unregister_buffer;
        output.submit = &submit;
        output.poll = &poll;
        output.cancel = &cancel;
        output.get_health = &get_health;
        output.reset = &reset;
        output.shutdown = &shutdown;
        return output;
    }
};

} // namespace

TEST(DeviceRuntime, FailedTeardownRetainsOwnershipAndRetriesOnlyPendingWork) {
    std::vector<std::uint32_t> calls;
    TeardownProbe first;
    first.id = 0;
    first.calls = &calls;
    first.shutdown_failure = RTFW_DEVICE_STATUS_RESET_REQUIRED;
    first.shutdown_failures_remaining = 1;
    TeardownProbe second;
    second.id = 1;
    second.calls = &calls;
    second.unregister_failure = RTFW_DEVICE_STATUS_LOST;
    second.unregister_failures_remaining = 1;

    rt::Runtime runtime;
    auto config = device_config(1, 1, 2);
    config.device_backend_capacity = 2;
    config.device_buffer_capacity = 2;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback({"cpu", &no_op_cpu, nullptr}),
        rt::Status::ok);

    rt::DeviceBackendHandle first_backend;
    rt::DeviceBackendHandle second_backend;
    ASSERT_EQ(
        runtime.register_device_backend(
            {"first", first.api()},
            first_backend),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_device_backend(
            {"second", second.api()},
            second_backend),
        rt::Status::ok);
    std::array<std::byte, 16> first_storage{};
    std::array<std::byte, 16> second_storage{};
    rt::DeviceBufferHandle first_buffer;
    rt::DeviceBufferHandle second_buffer;
    ASSERT_EQ(
        runtime.register_device_buffer(
            {"first.buffer", first_backend, first_storage},
            first_buffer),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_device_buffer(
            {"second.buffer", second_backend, second_storage},
            second_buffer),
        rt::Status::ok);
    std::array<std::byte, 8> registered_state{};
    registered_state.fill(std::byte{0x11});
    ASSERT_EQ(
        runtime.register_state(
            {"teardown.state", 1, registered_state}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    std::size_t checkpoint_size = 0;
    ASSERT_EQ(
        runtime.checkpoint_size(checkpoint_size),
        rt::Status::ok);
    std::vector<std::byte> checkpoint(checkpoint_size);
    rt::ArtifactWriteResult checkpoint_result;
    ASSERT_EQ(
        runtime.write_checkpoint(
            0,
            checkpoint,
            checkpoint_result),
        rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    calls.clear();
    EXPECT_EQ(runtime.stop(), rt::Status::device_lost);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::running);
    EXPECT_EQ(second.unregister_calls, 1u);
    EXPECT_EQ(second.shutdown_calls, 0u);
    EXPECT_EQ(second.registered_buffers, 1u);
    EXPECT_TRUE(second.initialized);
    EXPECT_EQ(first.unregister_calls, 1u);
    EXPECT_EQ(first.shutdown_calls, 1u);
    EXPECT_EQ(first.registered_buffers, 0u);
    EXPECT_TRUE(first.initialized);
    EXPECT_EQ(first.shutdown_with_registered_buffers, 0u);
    EXPECT_EQ(second.shutdown_with_registered_buffers, 0u);
    EXPECT_EQ(
        calls,
        (std::vector<std::uint32_t>{
            TeardownProbe::unregister_event + 1,
            TeardownProbe::unregister_event,
            TeardownProbe::shutdown_event,
        }));

    rt::StepResult step_result;
    EXPECT_EQ(
        runtime.step(
            rt::HostFrameContext{
                1,
                1ms,
                std::nullopt,
            },
            &step_result),
        rt::Status::invalid_state);
    auto health = rt::make_device_health();
    EXPECT_EQ(
        runtime.device_health(second_backend, health),
        rt::Status::invalid_state);
    EXPECT_EQ(
        runtime.reset_device(second_backend),
        rt::Status::invalid_state);
    registered_state.fill(std::byte{0x22});
    const auto state_before_restore = registered_state;
    EXPECT_EQ(
        runtime.restore_checkpoint(checkpoint),
        rt::Status::invalid_state);
    EXPECT_EQ(registered_state, state_before_restore);
    rt::ReplayResult replay_result;
    EXPECT_EQ(
        runtime.replay(
            checkpoint,
            {},
            nullptr,
            nullptr,
            &replay_result),
        rt::Status::invalid_state);
    EXPECT_EQ(registered_state, state_before_restore);
    std::array<rt::RuntimeTraceEvent, 64> teardown_trace{};
    rt::RuntimeTraceCursor teardown_cursor;
    rt::RuntimeTraceReadResult teardown_trace_result;
    ASSERT_EQ(
        runtime.read_trace(
            teardown_cursor,
            teardown_trace,
            teardown_trace_result),
        rt::Status::ok);
    EXPECT_EQ(
        std::count_if(
            teardown_trace.begin(),
            teardown_trace.begin() +
                static_cast<std::ptrdiff_t>(
                    teardown_trace_result.events_read),
            [](const rt::RuntimeTraceEvent& event) {
                return event.type ==
                    rt::RuntimeTraceEventType::stopped;
            }),
        0);

    calls.clear();
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::stopped);
    EXPECT_EQ(first.unregister_calls, 1u);
    EXPECT_EQ(first.shutdown_calls, 2u);
    EXPECT_EQ(second.unregister_calls, 2u);
    EXPECT_EQ(second.shutdown_calls, 1u);
    EXPECT_FALSE(first.initialized);
    EXPECT_FALSE(second.initialized);
    EXPECT_EQ(
        calls,
        (std::vector<std::uint32_t>{
            TeardownProbe::unregister_event + 1,
            TeardownProbe::shutdown_event + 1,
            TeardownProbe::shutdown_event,
        }));
    ASSERT_EQ(
        runtime.read_trace(
            teardown_cursor,
            teardown_trace,
            teardown_trace_result),
        rt::Status::ok);
    EXPECT_EQ(
        std::count_if(
            teardown_trace.begin(),
            teardown_trace.begin() +
                static_cast<std::ptrdiff_t>(
                    teardown_trace_result.events_read),
            [](const rt::RuntimeTraceEvent& event) {
                return event.type ==
                    rt::RuntimeTraceEventType::stopped;
            }),
        1);

    calls.clear();
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_TRUE(calls.empty());
}

TEST(DeviceRuntime, FailedStartRollbackRemainsRecoverableThroughStop) {
    std::vector<std::uint32_t> calls;
    TeardownProbe first;
    first.id = 0;
    first.calls = &calls;
    TeardownProbe second;
    second.id = 1;
    second.calls = &calls;
    second.initialize_failure = RTFW_DEVICE_STATUS_ERROR;
    second.initialize_failures_remaining = 1;
    second.initialize_failure_retains_ownership = true;
    second.shutdown_failure = RTFW_DEVICE_STATUS_RESET_REQUIRED;
    second.shutdown_failures_remaining = 1;

    rt::Runtime runtime;
    auto config = device_config(1, 1, 2);
    config.device_backend_capacity = 2;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback({"cpu", &no_op_cpu, nullptr}),
        rt::Status::ok);
    rt::DeviceBackendHandle ignored;
    ASSERT_EQ(
        runtime.register_device_backend(
            {"first", first.api()},
            ignored),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_device_backend(
            {"second", second.api()},
            ignored),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);

    EXPECT_EQ(runtime.start(), rt::Status::device_error);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
    EXPECT_FALSE(first.initialized);
    EXPECT_TRUE(second.initialized);
    EXPECT_EQ(first.shutdown_calls, 1u);
    EXPECT_EQ(second.shutdown_calls, 1u);
    EXPECT_EQ(
        calls,
        (std::vector<std::uint32_t>{
            TeardownProbe::initialize_event,
            TeardownProbe::initialize_event + 1,
            TeardownProbe::shutdown_event + 1,
            TeardownProbe::shutdown_event,
        }));
    EXPECT_EQ(runtime.start(), rt::Status::invalid_state);

    calls.clear();
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::stopped);
    EXPECT_EQ(first.shutdown_calls, 1u);
    EXPECT_EQ(second.shutdown_calls, 2u);
    EXPECT_FALSE(first.initialized);
    EXPECT_FALSE(second.initialized);
    EXPECT_EQ(
        calls,
        (std::vector<std::uint32_t>{
            TeardownProbe::shutdown_event + 1,
        }));
}

TEST(DeviceRuntime, FailedInitializeWithoutOwnershipCanRestart) {
    std::vector<std::uint32_t> calls;
    TeardownProbe first;
    first.id = 0;
    first.calls = &calls;
    TeardownProbe second;
    second.id = 1;
    second.calls = &calls;
    second.initialize_failure = RTFW_DEVICE_STATUS_ERROR;
    second.initialize_failures_remaining = 1;

    rt::Runtime runtime;
    auto config = device_config(1, 1, 2);
    config.device_backend_capacity = 2;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback({"cpu", &no_op_cpu, nullptr}),
        rt::Status::ok);
    rt::DeviceBackendHandle ignored;
    ASSERT_EQ(
        runtime.register_device_backend(
            {"first", first.api()},
            ignored),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_device_backend(
            {"second", second.api()},
            ignored),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);

    EXPECT_EQ(runtime.start(), rt::Status::device_error);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
    EXPECT_FALSE(first.initialized);
    EXPECT_FALSE(second.initialized);
    EXPECT_EQ(first.shutdown_calls, 1u);
    EXPECT_EQ(second.shutdown_calls, 1u);
    EXPECT_EQ(
        calls,
        (std::vector<std::uint32_t>{
            TeardownProbe::initialize_event,
            TeardownProbe::initialize_event + 1,
            TeardownProbe::shutdown_event + 1,
            TeardownProbe::shutdown_event,
        }));

    calls.clear();
    EXPECT_EQ(runtime.start(), rt::Status::ok);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::running);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::stopped);
}

TEST(DeviceRuntime, FailedRegistrationRollbackRemainsRecoverable) {
    std::vector<std::uint32_t> calls;
    CleanupMemoryProvider memory_provider;
    memory_provider.calls = &calls;
    TeardownProbe probe;
    probe.calls = &calls;
    probe.register_failure = RTFW_DEVICE_STATUS_ERROR;
    probe.register_failure_call = 2;
    probe.unregister_failure = RTFW_DEVICE_STATUS_LOST;
    probe.unregister_failures_remaining = 1;

    rt::Runtime runtime;
    auto config = device_config(1, 1, 2);
    config.device_backend_capacity = 1;
    config.device_buffer_capacity = 2;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(
        runtime.set_memory_provider(memory_provider.table()),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback({"cpu", &no_op_cpu, nullptr}),
        rt::Status::ok);
    rt::DeviceBackendHandle backend;
    ASSERT_EQ(
        runtime.register_device_backend(
            {"probe", probe.api()},
            backend),
        rt::Status::ok);
    std::array<std::byte, 16> first_storage{};
    std::array<std::byte, 16> second_storage{};
    rt::DeviceBufferHandle ignored;
    ASSERT_EQ(
        runtime.register_device_buffer(
            {"first", backend, first_storage},
            ignored),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_device_buffer(
            {"second", backend, second_storage},
            ignored),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);

    EXPECT_EQ(runtime.start(), rt::Status::device_error);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
    EXPECT_TRUE(probe.initialized);
    EXPECT_EQ(probe.registered_buffers, 1u);
    EXPECT_EQ(probe.unregister_calls, 1u);
    EXPECT_EQ(probe.shutdown_calls, 0u);
    // The retained device registration can still reference runtime-owned
    // execution state. Lower ownership categories must remain applied until a
    // checked retry resolves the M14.1 cleanup obligation.
    EXPECT_EQ(memory_provider.rollback_count, 0u);
    EXPECT_EQ(memory_provider.release_count, 0u);
    EXPECT_EQ(
        calls,
        (std::vector<std::uint32_t>{
            TeardownProbe::initialize_event,
            TeardownProbe::register_event,
            TeardownProbe::register_event,
            TeardownProbe::unregister_event,
        }));
    EXPECT_EQ(runtime.start(), rt::Status::invalid_state);

    calls.clear();
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::stopped);
    EXPECT_FALSE(probe.initialized);
    EXPECT_EQ(probe.registered_buffers, 0u);
    EXPECT_EQ(probe.unregister_calls, 2u);
    EXPECT_EQ(probe.shutdown_calls, 1u);
    EXPECT_EQ(memory_provider.rollback_count, 3u);
    EXPECT_EQ(memory_provider.release_count, 3u);
    EXPECT_EQ(
        calls,
        (std::vector<std::uint32_t>{
            TeardownProbe::unregister_event,
            TeardownProbe::shutdown_event,
            CleanupMemoryProvider::rollback_event +
                rt::memory_region_trace_storage.value,
            CleanupMemoryProvider::rollback_event +
                rt::memory_region_task_scratch.value,
            CleanupMemoryProvider::rollback_event +
                rt::memory_region_phase_scratch.value,
            CleanupMemoryProvider::release_event +
                rt::memory_region_trace_storage.value,
            CleanupMemoryProvider::release_event +
                rt::memory_region_task_scratch.value,
            CleanupMemoryProvider::release_event +
                rt::memory_region_phase_scratch.value,
        }));

    calls.clear();
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_TRUE(calls.empty());
}

TEST(DeviceRuntime, DeviceServicePolicyFailureRollsBackMemoryAfterJoinAndRetries) {
    rt::MockDeviceBackend backend({4, 1, 1, 1'000});
    CleanupMemoryProvider memory_provider;
    FailingDeviceServiceThreadProvider thread_provider;
    thread_provider.memory_provider = &memory_provider;
    rt::Runtime runtime;
    rt::detail::RuntimeThreadPolicyTestAccess::set_provider(
        runtime,
        thread_provider);
    ASSERT_EQ(runtime.configure(device_config(1, 1, 4)), rt::Status::ok);
    ASSERT_EQ(
        runtime.set_memory_provider(memory_provider.table()),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback({"cpu", &no_op_cpu, nullptr}),
        rt::Status::ok);
    rt::CpuMemoryPolicy policy;
    policy.thread_policy_count = 1;
    policy.thread_policies[0].role = rt::thread_role_device_service;
    policy.thread_policies[0].policy.requirement =
        rt::PolicyRequirement::strict;
    ASSERT_EQ(runtime.set_cpu_memory_policy(policy), rt::Status::ok);
    rt::DeviceBackendHandle backend_handle;
    ASSERT_EQ(
        runtime.register_device_backend(
            {"device-service-policy", backend.api()},
            backend_handle),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);

    EXPECT_EQ(runtime.start(), rt::Status::internal_error);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
    EXPECT_GE(thread_provider.device_join_count, 1u);
    EXPECT_EQ(thread_provider.rollback_count_seen_at_join, 0u);
    EXPECT_EQ(memory_provider.rollback_count, 3u);
    EXPECT_EQ(memory_provider.release_count, 0u);

    thread_provider.fail = false;
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(memory_provider.release_count, 3u);
}

TEST(DeviceRuntime, DeviceStackCleanupFailureDefersRollbackAndRetriesOnOwner) {
    rt::MockDeviceBackend backend({4, 1, 1, 1'000});
    CleanupMemoryProvider memory_provider;
    FailingDeviceServiceThreadProvider thread_provider;
    thread_provider.fail = false;
    thread_provider.memory_provider = &memory_provider;
    thread_provider.device_cleanup_failures_remaining = 1;
    rt::Runtime runtime;
    rt::detail::RuntimeThreadPolicyTestAccess::set_provider(
        runtime,
        thread_provider);
    ASSERT_EQ(runtime.configure(device_config(1, 1, 4)), rt::Status::ok);
    ASSERT_EQ(
        runtime.set_memory_provider(memory_provider.table()),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback({"cpu", &no_op_cpu, nullptr}),
        rt::Status::ok);
    rt::DeviceBackendHandle backend_handle;
    ASSERT_EQ(
        runtime.register_device_backend(
            {"device-stack-cleanup", backend.api()},
            backend_handle),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    EXPECT_EQ(runtime.stop(), rt::Status::internal_error);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::running);
    EXPECT_EQ(thread_provider.device_cleanup_count, 1u);
    EXPECT_EQ(thread_provider.device_join_count, 0u);
    EXPECT_NE(
        thread_provider.device_cleanup_thread,
        std::this_thread::get_id());
    EXPECT_EQ(memory_provider.rollback_count, 0u);
    EXPECT_EQ(memory_provider.release_count, 0u);

    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::stopped);
    EXPECT_EQ(thread_provider.device_cleanup_count, 2u);
    EXPECT_EQ(thread_provider.device_join_count, 1u);
    EXPECT_EQ(memory_provider.rollback_count, 3u);
    EXPECT_EQ(memory_provider.release_count, 3u);

    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(thread_provider.device_cleanup_count, 2u);
}

TEST(DeviceRuntime, UnresolvedStackCleanupDestructionFailsClosed) {
#if defined(__linux__) && GTEST_HAS_DEATH_TEST && \
    !defined(RTFW_DEVICE_TEST_ADDRESS_SANITIZER) && \
    !defined(RTFW_DEVICE_TEST_THREAD_SANITIZER)
    EXPECT_DEATH(
        {
            rt::MockDeviceBackend backend({4, 1, 1, 1'000});
            FailingDeviceServiceThreadProvider thread_provider;
            thread_provider.fail = false;
            thread_provider.device_cleanup_failures_remaining =
                std::numeric_limits<std::size_t>::max();
            rt::Runtime runtime;
            rt::detail::RuntimeThreadPolicyTestAccess::set_provider(
                runtime,
                thread_provider);
            ASSERT_EQ(
                runtime.configure(device_config(1, 1, 4)),
                rt::Status::ok);
            ASSERT_EQ(
                runtime.register_callback({"cpu", &no_op_cpu, nullptr}),
                rt::Status::ok);
            rt::DeviceBackendHandle backend_handle;
            ASSERT_EQ(
                runtime.register_device_backend(
                    {"device-stack-destructor", backend.api()},
                    backend_handle),
                rt::Status::ok);
            ASSERT_EQ(runtime.finalize(), rt::Status::ok);
            ASSERT_EQ(runtime.start(), rt::Status::ok);
        },
        "");
#endif
}

TEST(DeviceMock, BoundedQueueSaturatesWithoutBlocking) {
    rt::MockDeviceBackend backend({
        1,
        1,
        2,
        1'000,
    });
    auto api = backend.api();

    rtfw_device_init_config init{};
    init.struct_size = sizeof(init);
    init.abi_version = RTFW_DEVICE_ABI_VERSION;
    init.requested_in_flight = 1;
    ASSERT_EQ(
        api.initialize(api.instance, &init),
        RTFW_DEVICE_STATUS_OK);

    auto first = rt::make_device_submission();
    first.submission_id = 1;
    first.timeout_ns = 10'000;
    first.opcode = rt::mock_device_opcode_noop;
    auto second = first;
    second.submission_id = 2;
    EXPECT_EQ(
        api.submit(api.instance, &first),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(
        api.submit(api.instance, &second),
        RTFW_DEVICE_STATUS_QUEUE_FULL);

    rtfw_device_completion completion{};
    std::uint64_t count = 0;
    ASSERT_EQ(
        api.poll(api.instance, &completion, 1, &count),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(count, 0u);
    ASSERT_EQ(
        api.poll(api.instance, &completion, 1, &count),
        RTFW_DEVICE_STATUS_OK);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(completion.submission_id, 1u);
    EXPECT_EQ(completion.status, RTFW_DEVICE_STATUS_OK);

    auto canceled = first;
    canceled.submission_id = 3;
    ASSERT_EQ(
        api.submit(api.instance, &canceled),
        RTFW_DEVICE_STATUS_OK);
    ASSERT_EQ(
        api.cancel(api.instance, canceled.submission_id),
        RTFW_DEVICE_STATUS_OK);
    ASSERT_EQ(
        api.poll(api.instance, &completion, 1, &count),
        RTFW_DEVICE_STATUS_OK);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(completion.submission_id, 3u);
    EXPECT_EQ(
        completion.status,
        RTFW_DEVICE_STATUS_CANCELED);
    EXPECT_EQ(
        api.shutdown(api.instance),
        RTFW_DEVICE_STATUS_OK);
}

TEST(DeviceRuntime, DelayedCompletionReleasesOnlyDependentSuccessors) {
    rt::MockDeviceBackend backend({
        4,
        4,
        1,
        1'000,
    });
    const std::array faults{
        rt::MockDeviceFaultRule{
            1,
            rt::MockDeviceFault::delay,
            500'000,
        },
    };
    ASSERT_EQ(
        backend.set_fault_script(faults),
        RTFW_DEVICE_STATUS_OK);

    rt::Runtime runtime;
    ASSERT_EQ(
        runtime.configure(device_config(3, 2, 4)),
        rt::Status::ok);
    rt::DeviceBackendHandle backend_handle;
    ASSERT_EQ(
        runtime.register_device_backend(
            {"mock", backend.api()},
            backend_handle),
        rt::Status::ok);

    std::array<std::byte, 32> storage{};
    rt::DeviceBufferHandle buffer;
    ASSERT_EQ(
        runtime.register_device_buffer(
            {"output", backend_handle, storage},
            buffer),
        rt::Status::ok);

    std::atomic<std::uint32_t> sequence{0};
    FillCommand fill{
        buffer,
        storage.size(),
        0x5a,
        &sequence,
    };
    CpuOrderProbe dependent{
        &sequence,
        storage,
        std::byte{0x5a},
    };
    CpuOrderProbe independent{
        &sequence,
        {},
        {},
    };
    rt::PhaseHandle device_phase;
    rt::PhaseHandle dependent_phase;
    rt::PhaseHandle independent_phase;
    ASSERT_EQ(
        runtime.register_device_phase(
            {"fill", backend_handle, &submit_fill, &fill},
            device_phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback(
            {"dependent", &record_cpu_order, &dependent},
            dependent_phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_callback(
            {"independent", &record_cpu_order, &independent},
            independent_phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.add_dependency(device_phase, dependent_phase),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    rt::StepResult result;
    ASSERT_EQ(
        runtime.step({7, 1ms, std::nullopt}, &result),
        rt::Status::ok);
    EXPECT_EQ(result.callbacks_executed, 3u);
    EXPECT_EQ(fill.order, 1u);
    EXPECT_EQ(independent.order, 2u);
    EXPECT_EQ(dependent.order, 3u);
    EXPECT_TRUE(independent.contents_match);
    EXPECT_TRUE(dependent.contents_match);

    rt::RuntimeMetricSnapshot metrics;
    ASSERT_EQ(
        runtime.metrics_snapshot(
            rt::RuntimeMetricWindow::cumulative,
            nullptr,
            metrics),
        rt::Status::ok);
    EXPECT_EQ(
        metrics.samples[static_cast<std::size_t>(
            rt::RuntimeMetricId::device_submissions)].value,
        1u);
    EXPECT_EQ(
        metrics.samples[static_cast<std::size_t>(
            rt::RuntimeMetricId::device_completions)].value,
        1u);
    EXPECT_EQ(
        metrics.samples[static_cast<std::size_t>(
            rt::RuntimeMetricId::device_outstanding)].value,
        0u);
    auto health = rt::make_device_health();
    EXPECT_EQ(
        runtime.device_health(
            rt::DeviceBackendHandle{buffer.value},
            health),
        rt::Status::invalid_handle);

    std::array<rt::RuntimeTraceEvent, 64> trace{};
    rt::RuntimeTraceCursor cursor;
    rt::RuntimeTraceReadResult trace_result;
    ASSERT_EQ(
        runtime.read_trace(cursor, trace, trace_result),
        rt::Status::ok);
    std::size_t submitted = trace_result.events_read;
    std::size_t completed = trace_result.events_read;
    for (std::size_t index = 0;
         index < trace_result.events_read;
         ++index) {
        if (trace[index].type ==
            rt::RuntimeTraceEventType::device_submitted) {
            submitted = index;
        }
        if (trace[index].type ==
            rt::RuntimeTraceEventType::device_completed) {
            completed = index;
        }
    }
    EXPECT_LT(submitted, completed);
    ASSERT_LT(submitted, trace_result.events_read);
    ASSERT_LT(completed, trace_result.events_read);
    EXPECT_EQ(
        trace[submitted].producer,
        rt::RuntimeTraceProducer::worker);
    EXPECT_NE(
        trace[submitted].worker_index,
        std::numeric_limits<std::uint32_t>::max());
    EXPECT_EQ(
        trace[completed].producer,
        rt::RuntimeTraceProducer::device_service);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(DeviceRuntime, ConcurrentSubmissionsPreserveCausalPublication) {
    constexpr std::size_t phase_count = 8;
    constexpr std::size_t frame_count = 64;
    constexpr std::size_t submission_count = phase_count * frame_count;
    constexpr std::array<std::string_view, phase_count> names{
        "device.0",
        "device.1",
        "device.2",
        "device.3",
        "device.4",
        "device.5",
        "device.6",
        "device.7",
    };

    rt::MockDeviceBackend backend({
        64,
        1,
        1,
        1'000,
    });
    auto config = device_config(phase_count, 4, 64);
    config.executor_queue_capacity = 16;
    config.task_scratch_slots = 64;
    config.trace_capacity = 4096;

    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    rt::DeviceBackendHandle backend_handle;
    ASSERT_EQ(
        runtime.register_device_backend(
            {"mock", backend.api()},
            backend_handle),
        rt::Status::ok);
    std::array<rt::PhaseHandle, phase_count> phases{};
    for (std::size_t index = 0; index < phases.size(); ++index) {
        ASSERT_EQ(
            runtime.register_device_phase(
                {names[index],
                 backend_handle,
                 &submit_noop,
                 nullptr},
                phases[index]),
            rt::Status::ok);
    }
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    for (std::size_t frame = 0; frame < frame_count; ++frame) {
        ASSERT_EQ(
            runtime.step(
                {static_cast<std::uint64_t>(frame),
                 1ms,
                 std::nullopt}),
            rt::Status::ok);
    }

    rt::RuntimeMetricSnapshot metrics;
    ASSERT_EQ(
        runtime.metrics_snapshot(
            rt::RuntimeMetricWindow::cumulative,
            nullptr,
            metrics),
        rt::Status::ok);
    EXPECT_EQ(
        metrics.samples[static_cast<std::size_t>(
            rt::RuntimeMetricId::device_submissions)].value,
        submission_count);
    EXPECT_EQ(
        metrics.samples[static_cast<std::size_t>(
            rt::RuntimeMetricId::device_completions)].value,
        submission_count);

    std::array<rt::RuntimeTraceEvent, 4096> trace{};
    rt::RuntimeTraceCursor cursor;
    rt::RuntimeTraceReadResult trace_result;
    ASSERT_EQ(
        runtime.read_trace(cursor, trace, trace_result),
        rt::Status::ok);
    ASSERT_EQ(trace_result.lost_events, 0u);

    std::array<std::uint64_t, submission_count + 1> submitted{};
    std::array<std::uint64_t, submission_count + 1> completed{};
    for (std::size_t index = 0;
         index < trace_result.events_read;
         ++index) {
        const auto id = trace[index].value;
        if (id == 0 || id > submission_count) {
            continue;
        }
        if (trace[index].type ==
            rt::RuntimeTraceEventType::device_submitted) {
            submitted[static_cast<std::size_t>(id)] =
                trace[index].sequence;
        } else if (trace[index].type ==
                   rt::RuntimeTraceEventType::device_completed) {
            completed[static_cast<std::size_t>(id)] =
                trace[index].sequence;
        }
    }
    for (std::size_t id = 1; id <= submission_count; ++id) {
        ASSERT_NE(submitted[id], 0u);
        ASSERT_NE(completed[id], 0u);
        EXPECT_LT(submitted[id], completed[id]);
    }
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(DeviceRuntime, FaultsMapToStableRuntimeStatuses) {
    struct FaultCase {
        rt::MockDeviceFault fault;
        rt::Status expected;
        std::uint32_t expected_health;
    };
    constexpr std::array cases{
        FaultCase{
            rt::MockDeviceFault::timeout,
            rt::Status::device_timeout,
            RTFW_DEVICE_HEALTH_DEGRADED,
        },
        FaultCase{
            rt::MockDeviceFault::error,
            rt::Status::device_error,
            RTFW_DEVICE_HEALTH_DEGRADED,
        },
        FaultCase{
            rt::MockDeviceFault::loss,
            rt::Status::device_lost,
            RTFW_DEVICE_HEALTH_RESET_REQUIRED,
        },
    };

    for (const auto& test_case : cases) {
        SCOPED_TRACE(static_cast<int>(test_case.fault));
        rt::MockDeviceBackend backend({
            2,
            1,
            1,
            1'000,
        });
        const std::array faults{
            rt::MockDeviceFaultRule{
                1,
                test_case.fault,
                0,
            },
        };
        ASSERT_EQ(
            backend.set_fault_script(faults),
            RTFW_DEVICE_STATUS_OK);

        rt::Runtime runtime;
        ASSERT_EQ(
            runtime.configure(device_config(1, 1, 2)),
            rt::Status::ok);
        rt::DeviceBackendHandle backend_handle;
        ASSERT_EQ(
            runtime.register_device_backend(
                {"mock", backend.api()},
                backend_handle),
            rt::Status::ok);
        rt::PhaseHandle phase;
        ASSERT_EQ(
            runtime.register_device_phase(
                {"faulted", backend_handle, &submit_noop, nullptr},
                phase),
            rt::Status::ok);
        ASSERT_EQ(runtime.finalize(), rt::Status::ok);
        ASSERT_EQ(runtime.start(), rt::Status::ok);
        EXPECT_EQ(
            runtime.step({1, 1ms, std::nullopt}),
            test_case.expected);

        rt::DeviceHealth health = rt::make_device_health();
        ASSERT_EQ(
            runtime.device_health(backend_handle, health),
            rt::Status::ok);
        EXPECT_EQ(health.state, test_case.expected_health);
        if (test_case.fault == rt::MockDeviceFault::loss) {
            ASSERT_EQ(
                runtime.reset_device(backend_handle),
                rt::Status::ok);
            ASSERT_EQ(
                runtime.device_health(backend_handle, health),
                rt::Status::ok);
            EXPECT_EQ(health.state, RTFW_DEVICE_HEALTH_HEALTHY);
            EXPECT_EQ(
                runtime.step({2, 1ms, std::nullopt}),
                rt::Status::ok);
        }
        EXPECT_EQ(runtime.stop(), rt::Status::ok);
    }
}

TEST(DeviceRuntime, RejectsMalformedTablesAndAccountsMemory) {
    rt::MockDeviceBackend backend({
        2,
        2,
        1,
        1'000,
    });
    auto malformed = backend.api();
    malformed.poll = nullptr;

    rt::Runtime runtime;
    ASSERT_EQ(
        runtime.configure(device_config(1, 1, 2)),
        rt::Status::ok);
    rt::DeviceBackendHandle handle;
    EXPECT_EQ(
        runtime.register_device_backend(
            {"bad", malformed},
            handle),
        rt::Status::invalid_argument);
    ASSERT_EQ(
        runtime.register_device_backend(
            {"mock", backend.api()},
            handle),
        rt::Status::ok);
    std::array<std::byte, 8> storage{};
    rt::DeviceBufferHandle buffer;
    ASSERT_EQ(
        runtime.register_device_buffer(
            {"buffer", handle, storage},
            buffer),
        rt::Status::ok);
    rt::PhaseHandle phase;
    ASSERT_EQ(
        runtime.register_device_phase(
            {"noop", handle, &submit_noop, nullptr},
            phase),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);

    rt::MemoryPlan plan;
    ASSERT_TRUE(runtime.memory_plan(plan));
    EXPECT_EQ(plan.device_backend_count, 1u);
    EXPECT_EQ(plan.device_buffer_count, 1u);
    EXPECT_EQ(plan.device_outstanding_capacity, 2u);
    EXPECT_EQ(plan.device_completion_batch, 2u);
    EXPECT_GT(plan.device_control_bytes, 0u);
    EXPECT_GT(plan.device_backend_reported_bytes, 0u);
    EXPECT_EQ(
        plan.planned_bytes,
        plan.runtime_control_bytes +
            plan.executor_control_bytes +
            plan.device_control_bytes +
            plan.phase_scratch_total_bytes +
            plan.task_scratch_total_bytes +
            plan.trace_storage_bytes);
}

TEST(DeviceRuntime, D1RejectsUndeclaredDeterministicBackend) {
    rt::MockDeviceBackend backend;
    auto api = backend.api();
    api.get_capabilities =
        &report_nondeterministic_capabilities;

    auto config = device_config(1, 1, 4);
    config.determinism_tier =
        rt::DeterminismTier::schedule_independent;
    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    rt::DeviceBackendHandle backend_handle;
    ASSERT_EQ(
        runtime.register_device_backend(
            {"nondeterministic", api},
            backend_handle),
        rt::Status::ok);
    rt::PhaseHandle phase;
    ASSERT_EQ(
        runtime.register_device_phase(
            {
                "device",
                backend_handle,
                &submit_noop,
                nullptr,
            },
            phase),
        rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);

    rt::Runtime deterministic;
    ASSERT_EQ(deterministic.configure(config), rt::Status::ok);
    ASSERT_EQ(
        deterministic.register_device_backend(
            {"deterministic", backend.api()},
            backend_handle),
        rt::Status::ok);
    ASSERT_EQ(
        deterministic.register_device_phase(
            {
                "device",
                backend_handle,
                &submit_noop,
                nullptr,
            },
            phase),
        rt::Status::ok);
    ASSERT_EQ(deterministic.finalize(), rt::Status::ok);
    ASSERT_EQ(deterministic.start(), rt::Status::ok);
    ASSERT_EQ(
        deterministic.step({0, 1ms, std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(deterministic.stop(), rt::Status::ok);
}

TEST(DeviceRuntime, CrossInstanceDeviceStateIsIsolated) {
    constexpr std::size_t frame_count = 32;
    rt::MockDeviceBackend first_backend({4, 2, 1, 1'000});
    rt::MockDeviceBackend second_backend({4, 2, 1, 1'000});
    auto first_api = first_backend.api();
    auto second_api = second_backend.api();

    auto config = device_config(1, 2, 4);
    config.trace_capacity = 256;
    rt::Runtime first;
    rt::Runtime second;
    ASSERT_EQ(first.configure(config), rt::Status::ok);
    ASSERT_EQ(second.configure(config), rt::Status::ok);

    rt::DeviceBackendHandle first_backend_handle;
    rt::DeviceBackendHandle second_backend_handle;
    ASSERT_EQ(
        first.register_device_backend(
            {"first.mock", first_api},
            first_backend_handle),
        rt::Status::ok);
    ASSERT_EQ(
        second.register_device_backend(
            {"second.mock", second_api},
            second_backend_handle),
        rt::Status::ok);

    std::array<std::byte, 64> first_storage{};
    std::array<std::byte, 64> second_storage{};
    rt::DeviceBufferHandle first_buffer;
    rt::DeviceBufferHandle second_buffer;
    ASSERT_EQ(
        first.register_device_buffer(
            {"first.output", first_backend_handle, first_storage},
            first_buffer),
        rt::Status::ok);
    ASSERT_EQ(
        second.register_device_buffer(
            {"second.output", second_backend_handle, second_storage},
            second_buffer),
        rt::Status::ok);

    FillCommand first_fill{
        first_buffer,
        first_storage.size(),
        0x3c,
    };
    FillCommand second_fill{
        second_buffer,
        second_storage.size(),
        0xa7,
    };
    rt::PhaseHandle first_phase;
    rt::PhaseHandle second_phase;
    ASSERT_EQ(
        first.register_device_phase(
            {
                "first.fill",
                first_backend_handle,
                &submit_fill,
                &first_fill,
            },
            first_phase),
        rt::Status::ok);
    ASSERT_EQ(
        second.register_device_phase(
            {
                "second.fill",
                second_backend_handle,
                &submit_fill,
                &second_fill,
            },
            second_phase),
        rt::Status::ok);
    ASSERT_EQ(first.finalize(), rt::Status::ok);
    ASSERT_EQ(second.finalize(), rt::Status::ok);
    ASSERT_EQ(first.start(), rt::Status::ok);
    ASSERT_EQ(second.start(), rt::Status::ok);

    std::array<rt::Status, frame_count> first_statuses{};
    std::array<rt::Status, frame_count> second_statuses{};
    std::thread first_host([&] {
        for (std::size_t frame = 0; frame < frame_count; ++frame) {
            first_statuses[frame] = first.step(
                {static_cast<std::uint64_t>(frame),
                 1ms,
                 std::nullopt});
        }
    });
    std::thread second_host([&] {
        for (std::size_t frame = 0; frame < frame_count; ++frame) {
            second_statuses[frame] = second.step(
                {static_cast<std::uint64_t>(frame),
                 2ms,
                 std::nullopt});
        }
    });
    first_host.join();
    second_host.join();

    EXPECT_TRUE(std::all_of(
        first_statuses.begin(),
        first_statuses.end(),
        [](rt::Status status) {
            return status == rt::Status::ok;
        }));
    EXPECT_TRUE(std::all_of(
        second_statuses.begin(),
        second_statuses.end(),
        [](rt::Status status) {
            return status == rt::Status::ok;
        }));
    EXPECT_TRUE(std::all_of(
        first_storage.begin(),
        first_storage.end(),
        [](std::byte value) {
            return value == std::byte{0x3c};
        }));
    EXPECT_TRUE(std::all_of(
        second_storage.begin(),
        second_storage.end(),
        [](std::byte value) {
            return value == std::byte{0xa7};
        }));

    rt::RuntimeMetricSnapshot first_metrics;
    rt::RuntimeMetricSnapshot second_metrics;
    ASSERT_EQ(
        first.metrics_snapshot(
            rt::RuntimeMetricWindow::cumulative,
            nullptr,
            first_metrics),
        rt::Status::ok);
    ASSERT_EQ(
        second.metrics_snapshot(
            rt::RuntimeMetricWindow::cumulative,
            nullptr,
            second_metrics),
        rt::Status::ok);
    EXPECT_NE(
        first_metrics.metadata.runtime_id,
        second_metrics.metadata.runtime_id);
    const auto submissions =
        static_cast<std::size_t>(
            rt::RuntimeMetricId::device_submissions);
    const auto completions =
        static_cast<std::size_t>(
            rt::RuntimeMetricId::device_completions);
    EXPECT_EQ(first_metrics.samples[submissions].value, frame_count);
    EXPECT_EQ(second_metrics.samples[submissions].value, frame_count);
    EXPECT_EQ(first_metrics.samples[completions].value, frame_count);
    EXPECT_EQ(second_metrics.samples[completions].value, frame_count);

    rt::DeviceHealth first_health = rt::make_device_health();
    rt::DeviceHealth second_health = rt::make_device_health();
    ASSERT_EQ(
        first.device_health(first_backend_handle, first_health),
        rt::Status::ok);
    ASSERT_EQ(
        second.device_health(second_backend_handle, second_health),
        rt::Status::ok);
    EXPECT_EQ(first_health.submissions, frame_count);
    EXPECT_EQ(second_health.submissions, frame_count);

    ASSERT_EQ(first.stop(), rt::Status::ok);
    ASSERT_EQ(
        first_api.get_health(first_api.instance, &first_health),
        RTFW_DEVICE_STATUS_OK);
    ASSERT_EQ(
        second_api.get_health(second_api.instance, &second_health),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(first_health.state, RTFW_DEVICE_HEALTH_SHUTDOWN);
    EXPECT_EQ(second_health.state, RTFW_DEVICE_HEALTH_HEALTHY);
    EXPECT_EQ(
        second.step({frame_count, 2ms, std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(second.stop(), rt::Status::ok);
}

TEST(DeviceRuntime, DestructionShutsBackendDown) {
    rt::MockDeviceBackend backend({
        2,
        1,
        1,
        1'000,
    });
    auto api = backend.api();
    {
        rt::Runtime runtime;
        ASSERT_EQ(
            runtime.configure(device_config(1, 1, 2)),
            rt::Status::ok);
        rt::DeviceBackendHandle backend_handle;
        ASSERT_EQ(
            runtime.register_device_backend(
                {"mock", api},
                backend_handle),
            rt::Status::ok);
        rt::PhaseHandle phase;
        ASSERT_EQ(
            runtime.register_device_phase(
                {"noop", backend_handle, &submit_noop, nullptr},
                phase),
            rt::Status::ok);
        ASSERT_EQ(runtime.finalize(), rt::Status::ok);
        ASSERT_EQ(runtime.start(), rt::Status::ok);
        ASSERT_EQ(
            runtime.step({1, 1ms, std::nullopt}),
            rt::Status::ok);
    }

    auto health = rt::make_device_health();
    ASSERT_EQ(
        api.get_health(api.instance, &health),
        RTFW_DEVICE_STATUS_OK);
    EXPECT_EQ(health.state, RTFW_DEVICE_HEALTH_SHUTDOWN);
    EXPECT_EQ(health.outstanding, 0u);
}
