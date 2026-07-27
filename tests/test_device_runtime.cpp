#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include <rt/mock_device.hpp>
#include <rt/runtime.hpp>

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

} // namespace

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
