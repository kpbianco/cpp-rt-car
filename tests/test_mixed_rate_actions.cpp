#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>

#include <rt/loopback_backend.hpp>
#include <rt/runtime.hpp>

#include "rt/src/mixed_rate_actions.hpp"

namespace {

rt::CallbackResult noop_callback(
    void*,
    const rt::CallbackContext& context) {
    return context.rate_release
        ? rt::CallbackResult::ok
        : rt::CallbackResult::error;
}

struct ManualClock final : rt::RuntimeClock {
    std::uint64_t now = 1'000;
    std::uint64_t now_ns() noexcept override { return now; }
    rt::Status sleep_until_ns(std::uint64_t) noexcept override {
        return rt::Status::ok;
    }
};

inline constexpr std::size_t loopback_frame_bytes =
    sizeof(rt::SampledIoFrameHeader) + 8;

std::array<std::byte, loopback_frame_bytes> loopback_frame() {
    std::array<std::byte, loopback_frame_bytes> bytes{};
    rt::SampledIoFrameHeader header{};
    header.channel_identity = 101;
    header.sequence = 1;
    header.sample_count = 4;
    header.encoding = static_cast<std::uint32_t>(
        rt::SampledIoEncoding::signed_int16_le);
    header.timestamp_domain_identity = 7;
    header.sample_interval_ns = 1;
    header.trigger_identity = 404;
    header.trigger_sequence = 1;
    header.calibration_identity = 303;
    header.status = static_cast<std::uint32_t>(
        rt::SampledIoFrameStatus::produced);
    header.payload_checksum = rt::sampled_io_payload_checksum(
        std::span<const std::byte>(bytes).subspan(sizeof(header)));
    std::memcpy(bytes.data(), &header, sizeof(header));
    return bytes;
}

struct OptionalDeviceProbe {
    rt::DeviceCommandBatch declaration{};
    std::size_t calls = 0;
    std::uint64_t signal_value = 1;
};

rt::CallbackResult optional_device_callback(
    void* opaque,
    const rt::DeviceCallbackContext& context,
    rt::DeviceCommandBatch& batch) {
    auto& probe = *static_cast<OptionalDeviceProbe*>(opaque);
    if (!context.rate_release) {
        return rt::CallbackResult::error;
    }
    ++probe.calls;
    batch = probe.declaration;
    batch.timeout_ns = 50'000'000;
    batch.signals[0].value = ++probe.signal_value;
    return rt::CallbackResult::ok;
}

rt::MixedRateActionRecord action_record(std::uint64_t frame) {
    rt::MixedRateActionRecord record;
    record.runtime_id = 1;
    record.host_policy_version = 7;
    record.frame_index = frame;
    record.phase_index = std::numeric_limits<std::uint32_t>::max();
    return record;
}

} // namespace

TEST(MixedRateActions, NumericTablesAndRecordShapeAreClosed) {
    EXPECT_EQ(rt::mixed_rate_action_schema_version, 1u);
    EXPECT_EQ(rt::active_replay_schema_version, 1u);
    EXPECT_EQ(sizeof(rt::MixedRateActionRecord), 256u);
    EXPECT_EQ(
        static_cast<unsigned>(rt::MixedRateActionId::rate_execute), 1u);
    EXPECT_EQ(
        static_cast<unsigned>(rt::MixedRateActionId::runtime_stop), 11u);
    EXPECT_EQ(
        static_cast<unsigned>(rt::MixedRateActionReason::watchdog), 16u);
    EXPECT_EQ(
        static_cast<unsigned>(rt::MixedRateActionReason::cleanup_pending),
        19u);
    EXPECT_EQ(
        static_cast<unsigned>(rt::MixedRateActionStage::quarantined), 7u);
    EXPECT_TRUE(rt::detail::mixed_rate_action_valid(action_record(1)));

    auto malformed = action_record(1);
    malformed.reserved[0] = std::byte{1};
    EXPECT_FALSE(rt::detail::mixed_rate_action_valid(malformed));
    malformed = action_record(1);
    malformed.action = static_cast<rt::MixedRateActionId>(255);
    EXPECT_FALSE(rt::detail::mixed_rate_action_valid(malformed));
    malformed = action_record(1);
    malformed.action = rt::MixedRateActionId::runtime_stop;
    EXPECT_FALSE(rt::detail::mixed_rate_action_valid(malformed));
}

TEST(MixedRateActions, ZeroExactAndOneOverCapacityHaveExactLoss) {
    rt::detail::MixedRateActionRing zero(0);
    std::uint64_t assigned = std::numeric_limits<std::uint64_t>::max();
    EXPECT_FALSE(zero.emit(action_record(1), &assigned));
    EXPECT_EQ(assigned, 0u);
    EXPECT_EQ(zero.next_sequence(), 1u);
    EXPECT_EQ(zero.emitted(), 0u);
    EXPECT_EQ(zero.dropped(), 1u);

    rt::detail::MixedRateActionRing ring(2);
    EXPECT_TRUE(ring.emit(action_record(1)));
    EXPECT_TRUE(ring.emit(action_record(2)));
    EXPECT_TRUE(ring.emit(action_record(3)));
    EXPECT_EQ(ring.next_sequence(), 3u);
    EXPECT_EQ(ring.emitted(), 3u);
    EXPECT_EQ(ring.overwritten(), 1u);
    EXPECT_EQ(ring.oldest_sequence(3), 1u);
    rt::MixedRateActionRecord record;
    EXPECT_FALSE(ring.read_sequence(0, record));
    ASSERT_TRUE(ring.read_sequence(1, record));
    EXPECT_EQ(record.frame_index, 2u);
    ASSERT_TRUE(ring.read_sequence(2, record));
    EXPECT_EQ(record.frame_index, 3u);
    EXPECT_TRUE(ring.gap_free(1, 2));
    EXPECT_FALSE(ring.gap_free(0, 3));

    ring.restore_sequence(9);
    EXPECT_EQ(ring.next_sequence(), 9u);
    EXPECT_EQ(ring.emitted(), 0u);
    EXPECT_EQ(ring.overwritten(), 0u);
    EXPECT_EQ(ring.dropped(), 0u);
    EXPECT_FALSE(ring.read_sequence(1, record));

    ring.restore_sequence(std::numeric_limits<std::uint64_t>::max());
    EXPECT_FALSE(ring.emit(action_record(4), &assigned));
    EXPECT_EQ(assigned, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(
        ring.next_sequence(), std::numeric_limits<std::uint64_t>::max());
}

TEST(MixedRateActions, PolicyValidationIsTransactionalAndAccounted) {
    using namespace std::chrono_literals;
    rt::Runtime missing_active;
    ASSERT_EQ(missing_active.configure({}), rt::Status::ok);
    rt::MixedRateClosurePolicy closure{
        7,
        8,
        32,
        64 * 1024,
        16,
        rt::MixedRateOverflowPolicy::overwrite_committed,
        true,
        true,
        {},
    };
    ASSERT_EQ(
        missing_active.set_mixed_rate_closure_policy(closure),
        rt::Status::ok);
    EXPECT_EQ(missing_active.finalize(), rt::Status::invalid_config);

    ManualClock clock;
    rt::Runtime runtime(clock);
    rt::RuntimeConfig config;
    config.callback_capacity = 4;
    config.executor_queue_capacity = 4;
    config.task_scratch_slots = 4;
    config.snapshot_max_bytes = 64 * 1024;
    config.memory_budget_bytes = 4 * 1024 * 1024;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(runtime.set_rate_execution_policy({4}), rt::Status::ok);

    auto malformed = closure;
    malformed.host_policy_version = 0;
    EXPECT_EQ(
        runtime.set_mixed_rate_closure_policy(malformed),
        rt::Status::invalid_argument);
    ASSERT_EQ(
        runtime.set_mixed_rate_closure_policy(closure),
        rt::Status::ok);
    EXPECT_EQ(
        runtime.set_mixed_rate_closure_policy(closure),
        rt::Status::invalid_state);

    rt::PhaseHandle phase;
    rt::RateDomainHandle domain;
    ASSERT_EQ(
        runtime.register_callback({"phase", &noop_callback, nullptr}, phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_rate_domain(
            {"domain", 100, 1, 100, 10},
            domain),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.bind_phase_to_rate_domain(phase, domain),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);

    rt::MemoryPlan plan;
    ASSERT_TRUE(runtime.memory_plan(plan));
    EXPECT_EQ(plan.mixed_rate_action_capacity, closure.action_capacity);
    EXPECT_GT(plan.mixed_rate_action_slot_bytes, 0u);
    EXPECT_GT(plan.mixed_rate_action_storage_bytes, 0u);
    EXPECT_GT(plan.rate_checkpoint_state_bytes, 0u);
    EXPECT_GT(plan.mixed_rate_replay_control_bytes, 0u);
    EXPECT_EQ(
        plan.planned_bytes,
        plan.runtime_control_bytes +
            plan.executor_control_bytes +
            plan.device_control_bytes +
            plan.phase_scratch_total_bytes +
            plan.task_scratch_total_bytes +
            plan.trace_storage_bytes);

    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(
        runtime.step({1, 100ns, std::nullopt, std::uint64_t{1'000}}),
        rt::Status::ok);
    rt::MixedRateActionMetadata metadata;
    ASSERT_EQ(
        runtime.mixed_rate_action_metadata(metadata),
        rt::Status::ok);
    EXPECT_EQ(metadata.counter_count, 3u);
    EXPECT_EQ(metadata.host_policy_version, closure.host_policy_version);
    EXPECT_EQ(metadata.capacity, closure.action_capacity);
    EXPECT_EQ(metadata.next_sequence, 1u);
    EXPECT_TRUE(metadata.replay_eligible);

    rt::MixedRateActionCursor malformed_cursor;
    malformed_cursor.reserved0 = 1;
    rt::MixedRateActionReadResult read_result;
    std::array<rt::MixedRateActionRecord, 2> records{};
    EXPECT_EQ(
        runtime.read_mixed_rate_actions(
            malformed_cursor, records, read_result),
        rt::Status::invalid_argument);
    EXPECT_EQ(malformed_cursor.runtime_id, 0u);
    EXPECT_EQ(malformed_cursor.next_sequence, 0u);

    rt::MixedRateActionCursor cursor;
    ASSERT_EQ(
        runtime.read_mixed_rate_actions(cursor, records, read_result),
        rt::Status::ok);
    ASSERT_EQ(read_result.records_read, 1u);
    EXPECT_EQ(records[0].action, rt::MixedRateActionId::rate_execute);
    EXPECT_EQ(
        records[0].phase_index,
        std::numeric_limits<std::uint32_t>::max());
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(MixedRateActions, OptionalDeviceSheddingSkipsOwnershipAndRecovers) {
    using namespace std::chrono_literals;

    ManualClock clock;
    clock.now = 200'000'000;
    rt::SampledIoLoopbackBackend backend({4, 2, 4096, 1, 7});
    ASSERT_EQ(
        backend.add_route({17, 101, 202, 7, 303, 404}),
        rt::Status::ok);
    rt::Runtime runtime(clock);
    rt::RuntimeConfig config;
    config.callback_capacity = 2;
    config.executor_queue_capacity = 4;
    config.task_scratch_slots = 4;
    config.device_backend_capacity = 1;
    config.device_buffer_capacity = 1;
    config.device_outstanding_capacity = 4;
    config.device_completion_batch = 4;
    config.snapshot_max_bytes = 64 * 1024;
    config.memory_budget_bytes = 4 * 1024 * 1024;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(
        runtime.set_rate_execution_policy({8, 9, 1, 1, 16}),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.set_mixed_rate_closure_policy({
            9,
            32,
            0,
            0,
            0,
            rt::MixedRateOverflowPolicy::overwrite_committed,
            false,
            true,
            {},
        }),
        rt::Status::ok);

    rt::DeviceBackendHandle backend_handle;
    ASSERT_EQ(
        runtime.register_device_backend(
            backend.hal_v2_registration(), backend_handle),
        rt::Status::ok);
    rt::DeviceMemoryDomainHandle memory_domain;
    rt::HalV2MemoryDomain memory_description;
    ASSERT_TRUE(runtime.device_memory_domain_at(
        backend_handle, 0, memory_domain, memory_description));
    alignas(64) std::array<std::byte, loopback_frame_bytes * 2> storage{};
    const auto source = loopback_frame();
    std::copy(source.begin(), source.end(), storage.begin());
    rt::DeviceBufferHandle buffer;
    ASSERT_EQ(
        runtime.register_device_buffer(
            {
                "optional.device.buffer",
                backend_handle,
                memory_domain,
                storage,
                {},
                storage.size(),
                rt::HalV2MemoryOwnership::borrowed_host,
                RTFW_DEVICE_BUFFER_HOST_READ |
                    RTFW_DEVICE_BUFFER_HOST_WRITE |
                    RTFW_DEVICE_BUFFER_DEVICE_READ |
                    RTFW_DEVICE_BUFFER_DEVICE_WRITE,
                rt::HalV2MemoryCoherency::host_coherent,
                rt::hal_v2_memory_sync_none,
            },
            buffer),
        rt::Status::ok);
    rt::DeviceTimelineHandle timeline;
    ASSERT_EQ(
        runtime.register_device_timeline(
            {"optional.device.timeline", backend_handle, 0}, timeline),
        rt::Status::ok);

    rt::PhaseHandle mandatory_phase;
    rt::PhaseHandle optional_phase;
    ASSERT_EQ(
        runtime.register_callback(
            {"mandatory.cpu", &noop_callback, nullptr}, mandatory_phase),
        rt::Status::ok);
    OptionalDeviceProbe probe;
    probe.declaration.command_count = 1;
    probe.declaration.signal_count = 1;
    probe.declaration.commands[0].kind = static_cast<std::uint32_t>(
        rt::HalV2CommandKind::dispatch);
    probe.declaration.commands[0].opcode = 17;
    probe.declaration.commands[0].buffer_count = 2;
    probe.declaration.commands[0].buffers[0] = {
        buffer.value,
        RTFW_DEVICE_ACCESS_READ,
        0,
        0,
        loopback_frame_bytes,
    };
    probe.declaration.commands[0].buffers[1] = {
        buffer.value,
        RTFW_DEVICE_ACCESS_WRITE,
        0,
        loopback_frame_bytes,
        loopback_frame_bytes,
    };
    probe.declaration.signals[0].timeline_handle = timeline.value;
    ASSERT_EQ(
        runtime.register_device_batch_phase(
            {
                "optional.device",
                backend_handle,
                &optional_device_callback,
                &probe,
                probe.declaration,
            },
            optional_phase),
        rt::Status::ok);

    rt::RateDomainHandle mandatory_domain;
    rt::RateDomainHandle optional_domain;
    ASSERT_EQ(
        runtime.register_rate_domain(
            {
                "mandatory.rate",
                100'000'000,
                1,
                50'000'000,
                10'000'000,
                rt::RateCriticality::critical,
                false,
                rt::RateLateAction::skip,
                0,
            },
            mandatory_domain),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_rate_domain(
            {
                "optional.device.rate",
                100'000'000,
                1,
                100'000'000,
                0,
                rt::RateCriticality::background,
                true,
                rt::RateLateAction::fail,
                0,
            },
            optional_domain),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.bind_phase_to_rate_domain(
            mandatory_phase, mandatory_domain),
        rt::Status::ok);
    const std::array roles{
        rt::DeviceRatePayloadRole::input,
        rt::DeviceRatePayloadRole::output,
    };
    ASSERT_EQ(
        runtime.bind_device_phase_to_rate_domain(
            {optional_phase, optional_domain, 80'000'000, 1, roles}),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok) << runtime.last_error();
    ASSERT_EQ(runtime.start(), rt::Status::ok);

    rt::StepResult late;
    ASSERT_EQ(
        runtime.step(
            {1, 100ms, std::nullopt, std::uint64_t{1'000}}, &late),
        rt::Status::ok) << runtime.last_error();
    EXPECT_EQ(probe.calls, 0u);
    EXPECT_EQ(backend.stats().submissions, 0u);
    EXPECT_EQ(late.rate.shed_transitions, 1u);
    EXPECT_EQ(late.rate.shed_domain_releases, 1u);

    clock.now = 100'001'000;
    rt::StepResult recovered;
    ASSERT_EQ(
        runtime.step(
            {2, 100ms, std::nullopt, std::uint64_t{100'001'000}},
                     &recovered),
        rt::Status::ok) << runtime.last_error();
    EXPECT_EQ(probe.calls, 1u);
    EXPECT_EQ(backend.stats().submissions, 1u);
    EXPECT_EQ(recovered.rate.recovery_transitions, 1u);

    rt::MixedRateActionCursor cursor;
    std::array<rt::MixedRateActionRecord, 32> actions{};
    rt::MixedRateActionReadResult read;
    ASSERT_EQ(
        runtime.read_mixed_rate_actions(cursor, actions, read),
        rt::Status::ok);
    ASSERT_EQ(read.lost_records, 0u);
    bool saw_transition_shed = false;
    bool saw_skipped_device = false;
    bool saw_recovery = false;
    bool saw_device_terminal = false;
    for (std::size_t index = 0; index < read.records_read; ++index) {
        const auto& action = actions[index];
        saw_transition_shed = saw_transition_shed ||
            (action.action == rt::MixedRateActionId::rate_shed &&
             action.reason == rt::MixedRateActionReason::late_threshold &&
             action.rate_domain_registration_index == 0 &&
             action.shed_state_before == 0 &&
             action.shed_state_after != 0);
        saw_skipped_device = saw_skipped_device ||
            (action.action == rt::MixedRateActionId::rate_shed &&
             action.reason == rt::MixedRateActionReason::already_shed &&
             action.rate_domain_registration_index == 1);
        saw_recovery = saw_recovery ||
            (action.action == rt::MixedRateActionId::rate_recover &&
             action.reason == rt::MixedRateActionReason::on_time_threshold &&
             action.shed_state_before != 0 &&
             action.shed_state_after == 0);
        saw_device_terminal = saw_device_terminal ||
            (action.action == rt::MixedRateActionId::device_terminal &&
             action.rate_domain_registration_index == 1 &&
             action.terminal_status == static_cast<std::int32_t>(
                 rt::Status::ok));
    }
    EXPECT_TRUE(saw_transition_shed);
    EXPECT_TRUE(saw_skipped_device);
    EXPECT_TRUE(saw_recovery);
    EXPECT_TRUE(saw_device_terminal);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}
