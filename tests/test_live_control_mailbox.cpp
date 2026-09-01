#include <gtest/gtest.h>

#include <array>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <thread>
#include <vector>

#include <rt/runtime.hpp>

#include "rt/src/live_control_mailbox.hpp"

namespace {

constexpr std::uint64_t kMailbox = 101;
constexpr std::uint64_t kProducer = 202;

struct ManualClock final : rt::RuntimeClock {
    std::uint64_t now_ns() noexcept override { return 1'000; }
    rt::Status sleep_until_ns(std::uint64_t) noexcept override {
        return rt::Status::ok;
    }
    bool supports_absolute_sleep() const noexcept override { return true; }
};

rt::CallbackResult noop_callback(
    void*,
    const rt::CallbackContext&) {
    return rt::CallbackResult::ok;
}

rt::CallbackResult failing_callback(
    void*,
    const rt::CallbackContext&) {
    return rt::CallbackResult::error;
}

rt::CallbackResult noop_replay_input(
    void*,
    const rt::ReplayInputView&) {
    return rt::CallbackResult::ok;
}

struct GenerationProbe {
    std::size_t calls = 0;
    std::uint64_t generation_identity = 0;
    rt::LiveControlBoundaryTarget target{};
    std::size_t record_count = 0;
    std::array<std::uint64_t, 4> mailbox_sequences{};
    std::array<std::uint64_t, 4> mailbox_identities{};
    std::array<rt::LiveControlUpdateKind, 4> update_kinds{};
    std::array<std::byte, 4> first_payload_bytes{};
};

rt::CallbackResult capture_generation_callback(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& probe = *static_cast<GenerationProbe*>(user_data);
    ++probe.calls;
    probe.generation_identity = 0;
    probe.record_count = 0;
    if (!context.live_control) {
        return rt::CallbackResult::ok;
    }
    probe.generation_identity = context.live_control->generation_identity;
    probe.target = context.live_control->target;
    probe.record_count = context.live_control->records.size();
    for (std::size_t index = 0;
         index < context.live_control->records.size() &&
             index < probe.mailbox_sequences.size();
         ++index) {
        const auto& record = context.live_control->records[index];
        probe.mailbox_sequences[index] = record.record.mailbox_sequence;
        probe.mailbox_identities[index] = record.record.mailbox_identity;
        probe.update_kinds[index] = record.record.update_kind;
        probe.first_payload_bytes[index] = record.payload.empty()
            ? std::byte{0}
            : record.payload.front();
    }
    return rt::CallbackResult::ok;
}

rt::CallbackResult capture_generation_then_fail_callback(
    void* user_data,
    const rt::CallbackContext& context) {
    (void)capture_generation_callback(user_data, context);
    return rt::CallbackResult::error;
}

struct ReentrantAdmissionProbe {
    rt::Runtime* runtime = nullptr;
    rt::LiveControlProducerHandle handle{};
    rt::LiveControlUpdateRecord update{};
    std::array<std::byte, 1> payload{std::byte{0x33}};
    rt::LiveControlAdmissionResult result =
        rt::LiveControlAdmissionResult::accepted;
    rt::Status status = rt::Status::internal_error;
};

rt::CallbackResult reentrant_admission_callback(
    void* user_data,
    const rt::CallbackContext&) {
    auto& probe = *static_cast<ReentrantAdmissionProbe*>(user_data);
    probe.status = probe.runtime->stage_live_control_update(
        probe.handle,
        probe.update,
        probe.payload,
        probe.result);
    return rt::CallbackResult::ok;
}

rt::LiveControlPolicy policy(
    std::uint32_t mailbox_capacity = 1,
    std::uint32_t producer_capacity = 1,
    std::uint32_t record_capacity = 2,
    std::uint32_t payload_bytes = 16) {
    rt::LiveControlPolicy value;
    value.policy_identity = 0x4d323201;
    value.mailbox_capacity = mailbox_capacity;
    value.producer_capacity = producer_capacity;
    value.record_capacity = record_capacity;
    value.payload_bytes_per_record = payload_bytes;
    value.total_payload_storage_bytes =
        static_cast<std::uint64_t>(record_capacity) * payload_bytes;
    return value;
}

rt::LiveControlClosurePolicy closure_policy(bool replay = false) {
    rt::LiveControlClosurePolicy value;
    value.policy_identity = 0x4d323203;
    value.action_capacity = 128;
    if (replay) {
        value.retained_generation_capacity = 16;
        value.retained_record_capacity = 32;
        value.retained_payload_bytes = 1'024;
        value.replay_record_capacity = 128;
        value.replay_max_bytes = 64 * 1'024;
        value.replay_enabled = true;
    }
    return value;
}

rt::LiveControlMailboxRegistration mailbox(
    std::uint64_t identity = kMailbox,
    std::uint32_t record_capacity = 2,
    std::uint32_t payload_bytes = 16) {
    rt::LiveControlMailboxRegistration value;
    value.mailbox_identity = identity;
    value.record_capacity = record_capacity;
    value.payload_bytes_per_record = payload_bytes;
    return value;
}

rt::LiveControlProducerRegistration producer(
    std::uint64_t mailbox_identity = kMailbox,
    std::uint64_t producer_identity = kProducer,
    std::uint64_t first_sequence = 1) {
    rt::LiveControlProducerRegistration value;
    value.mailbox_identity = mailbox_identity;
    value.producer_identity = producer_identity;
    value.first_sequence = first_sequence;
    return value;
}

void configure_live_control(
    rt::Runtime& runtime,
    std::uint32_t record_capacity = 2,
    std::uint32_t payload_bytes = 16,
    std::uint64_t first_sequence = 1) {
    ASSERT_EQ(
        runtime.set_live_control_policy(
            policy(1, 1, record_capacity, payload_bytes)),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_live_control_mailbox(
            mailbox(kMailbox, record_capacity, payload_bytes)),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_live_control_producer(
            producer(kMailbox, kProducer, first_sequence)),
        rt::Status::ok);
}

rt::LiveControlProducerHandle finalized_handle(rt::Runtime& runtime) {
    rt::LiveControlProducerHandle handle;
    EXPECT_EQ(runtime.finalize(), rt::Status::ok);
    EXPECT_EQ(
        runtime.live_control_producer_handle(kMailbox, kProducer, handle),
        rt::Status::ok);
    EXPECT_TRUE(handle.valid());
    return handle;
}

rt::LiveControlUpdateRecord host_update(
    const rt::LiveControlProducerHandle& handle,
    std::uint64_t producer_sequence,
    std::span<const std::byte> payload,
    std::uint64_t frame = 7) {
    rt::LiveControlUpdateRecord update;
    update.runtime_id = handle.runtime_id;
    update.configuration_generation = handle.configuration_generation;
    update.mailbox_identity = handle.mailbox_identity;
    update.producer_identity = handle.producer_identity;
    update.producer_sequence = producer_sequence;
    update.target_frame_index = frame;
    update.payload_bytes = static_cast<std::uint32_t>(payload.size());
    update.payload_digest = rt::live_control_payload_digest(payload);
    return update;
}

rt::ObservabilityMetadata metadata(rt::Runtime& runtime) {
    rt::ObservabilityMetadata value;
    EXPECT_EQ(runtime.observability_metadata(value), rt::Status::ok);
    return value;
}

struct CompatibilityIds {
    std::uint64_t config = 0;
    std::uint64_t graph = 0;
    std::uint64_t replay = 0;
};

std::vector<std::byte> checkpoint_artifact(rt::Runtime& runtime) {
    std::size_t required = 0;
    EXPECT_EQ(runtime.checkpoint_size(required), rt::Status::ok);
    std::vector<std::byte> checkpoint(required);
    rt::ArtifactWriteResult write;
    EXPECT_EQ(
        runtime.write_checkpoint(0, checkpoint, write),
        rt::Status::ok);
    return checkpoint;
}

CompatibilityIds compatibility_ids(rt::Runtime& runtime) {
    const auto observable = metadata(runtime);
    const auto checkpoint = checkpoint_artifact(runtime);
    rt::CheckpointMetadata inspected;
    EXPECT_EQ(
        rt::inspect_checkpoint_artifact(checkpoint, inspected),
        rt::Status::ok);
    return {observable.config_id, inspected.graph_id, inspected.replay_id};
}

} // namespace

TEST(LiveControlMailbox, PublicLayoutAndDisabledPathRemainAdditive) {
    EXPECT_EQ(sizeof(rt::LiveControlPolicy), 56u);
    EXPECT_EQ(sizeof(rt::LiveControlMailboxRegistration), 40u);
    EXPECT_EQ(sizeof(rt::LiveControlProducerRegistration), 40u);
    EXPECT_EQ(sizeof(rt::LiveControlProducerHandle), 40u);
    EXPECT_EQ(sizeof(rt::LiveControlUpdateRecord), 128u);
    EXPECT_EQ(sizeof(rt::LiveControlMailboxInfo), 128u);
    EXPECT_EQ(sizeof(rt::LiveControlBoundaryTarget), 40u);
    EXPECT_EQ(sizeof(rt::LiveControlCommitInfo), 128u);
    EXPECT_EQ(sizeof(rt::LiveControlRecordStatusInfo), 80u);
    EXPECT_EQ(alignof(rt::LiveControlPolicy), 8u);
    EXPECT_EQ(alignof(rt::LiveControlMailboxRegistration), 8u);
    EXPECT_EQ(alignof(rt::LiveControlProducerRegistration), 8u);
    EXPECT_EQ(alignof(rt::LiveControlProducerHandle), 8u);
    EXPECT_EQ(alignof(rt::LiveControlUpdateRecord), 8u);
    EXPECT_EQ(alignof(rt::LiveControlMailboxInfo), 8u);

    const rt::LiveControlPolicy disabled;
    EXPECT_EQ(disabled.schema_version, rt::live_control_schema_version);
    EXPECT_EQ(disabled.struct_size, sizeof(disabled));
    EXPECT_EQ(disabled.mailbox_capacity, 0u);

    rt::Runtime baseline;
    ASSERT_EQ(baseline.finalize(), rt::Status::ok);
    const auto baseline_metadata = compatibility_ids(baseline);
    rt::MemoryPlan baseline_plan;
    ASSERT_TRUE(baseline.memory_plan(baseline_plan));
    EXPECT_FALSE(baseline.live_control_enabled());
    EXPECT_EQ(baseline_plan.live_control_control_bytes, 0u);

    rt::Runtime explicit_disabled;
    ASSERT_EQ(
        explicit_disabled.set_live_control_policy(disabled),
        rt::Status::ok);
    EXPECT_EQ(
        explicit_disabled.set_live_control_policy(disabled),
        rt::Status::invalid_state);
    const rt::LiveControlClosurePolicy disabled_closure;
    ASSERT_EQ(
        explicit_disabled.set_live_control_closure_policy(disabled_closure),
        rt::Status::ok);
    EXPECT_EQ(
        explicit_disabled.set_live_control_closure_policy(disabled_closure),
        rt::Status::invalid_state);
    ASSERT_EQ(explicit_disabled.finalize(), rt::Status::ok);
    const auto disabled_metadata = compatibility_ids(explicit_disabled);
    rt::MemoryPlan disabled_plan;
    ASSERT_TRUE(explicit_disabled.memory_plan(disabled_plan));
    EXPECT_FALSE(explicit_disabled.live_control_enabled());
    EXPECT_EQ(disabled_plan.live_control_mailbox_count, 0u);
    EXPECT_EQ(disabled_plan.live_control_control_bytes, 0u);
    EXPECT_EQ(
        disabled_plan.runtime_control_bytes,
        baseline_plan.runtime_control_bytes);
    EXPECT_EQ(disabled_plan.planned_bytes, baseline_plan.planned_bytes);
    EXPECT_EQ(disabled_metadata.config, baseline_metadata.config);
    EXPECT_EQ(disabled_metadata.graph, baseline_metadata.graph);
    EXPECT_EQ(disabled_metadata.replay, baseline_metadata.replay);
}

TEST(LiveControlMailbox, ConfigurationCopiesAndClosesEveryCapacityBoundary) {
    rt::Runtime baseline;
    ASSERT_EQ(baseline.finalize(), rt::Status::ok);
    rt::MemoryPlan baseline_plan;
    ASSERT_TRUE(baseline.memory_plan(baseline_plan));

    rt::Runtime runtime;
    auto configured_policy = policy(1, 1, 2, 8);
    ASSERT_EQ(
        runtime.set_live_control_policy(configured_policy),
        rt::Status::ok);
    configured_policy.policy_identity = 0;

    auto malformed_mailbox = mailbox(kMailbox, 2, 8);
    malformed_mailbox.reserved.front() = std::byte{1};
    EXPECT_EQ(
        runtime.register_live_control_mailbox(malformed_mailbox),
        rt::Status::invalid_argument);
    ASSERT_EQ(
        runtime.register_live_control_mailbox(mailbox(kMailbox, 2, 8)),
        rt::Status::ok);
    EXPECT_EQ(
        runtime.register_live_control_mailbox(mailbox(kMailbox, 1, 8)),
        rt::Status::capacity_exceeded);
    EXPECT_EQ(
        runtime.register_live_control_producer(producer(999, kProducer)),
        rt::Status::invalid_argument);
    ASSERT_EQ(
        runtime.register_live_control_producer(producer()),
        rt::Status::ok);
    EXPECT_EQ(
        runtime.register_live_control_producer(producer()),
        rt::Status::capacity_exceeded);

    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    EXPECT_TRUE(runtime.live_control_enabled());
    EXPECT_EQ(runtime.live_control_mailbox_count(), 1u);
    EXPECT_EQ(runtime.live_control_producer_count(), 1u);
    EXPECT_EQ(
        runtime.register_live_control_mailbox(mailbox()),
        rt::Status::invalid_state);

    rt::MemoryPlan plan;
    ASSERT_TRUE(runtime.memory_plan(plan));
    EXPECT_EQ(plan.live_control_mailbox_count, 1u);
    EXPECT_EQ(plan.live_control_producer_count, 1u);
    EXPECT_EQ(plan.live_control_record_capacity, 2u);
    EXPECT_EQ(plan.live_control_payload_storage_bytes, 16u);
    EXPECT_GT(plan.live_control_control_bytes, 16u);
    EXPECT_EQ(
        plan.runtime_control_bytes,
        baseline_plan.runtime_control_bytes +
            plan.live_control_control_bytes);
    EXPECT_EQ(
        plan.planned_bytes,
        baseline_plan.planned_bytes + plan.live_control_control_bytes);

    rt::Runtime wider_payload;
    configure_live_control(wider_payload, 2, 16);
    (void)finalized_handle(wider_payload);
    rt::MemoryPlan wider_plan;
    ASSERT_TRUE(wider_payload.memory_plan(wider_plan));
    // Eight additional bytes in each of two slots are present once in
    // staging and once in each inactive/active generation store.
    EXPECT_EQ(
        wider_plan.live_control_control_bytes -
            plan.live_control_control_bytes,
        48u);
}

TEST(LiveControlMailbox, FailedBudgetFinalizationIsTransactionalAndRetryable) {
    rt::Runtime runtime;
    rt::RuntimeConfig constrained;
    constrained.memory_budget_bytes = 1;
    ASSERT_EQ(runtime.configure(constrained), rt::Status::ok);
    configure_live_control(runtime, 2, 8);
    EXPECT_EQ(runtime.finalize(), rt::Status::invalid_config);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::configuring);
    EXPECT_FALSE(runtime.live_control_enabled());

    rt::RuntimeConfig corrected;
    ASSERT_EQ(runtime.configure(corrected), rt::Status::ok);
    const auto handle = finalized_handle(runtime);
    EXPECT_TRUE(handle.valid());
    EXPECT_TRUE(runtime.live_control_enabled());
}

TEST(LiveControlMailbox, CopiesBeforePublicationAndRejectsWithoutOverwrite) {
    rt::Runtime runtime;
    configure_live_control(runtime, 2, 8);
    const auto handle = finalized_handle(runtime);

    std::array payload{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    auto first = host_update(handle, 1, payload, 11);
    rt::LiveControlAdmissionResult result;
    ASSERT_EQ(
        runtime.stage_live_control_update(handle, first, payload, result),
        rt::Status::ok);
    EXPECT_EQ(result, rt::LiveControlAdmissionResult::accepted);
    payload.fill(std::byte{9});

    rt::LiveControlUpdateRecord published;
    ASSERT_TRUE(runtime.live_control_record_at(kMailbox, 1, published));
    EXPECT_EQ(published.mailbox_sequence, 1u);
    EXPECT_EQ(published.target_frame_index, 11u);
    EXPECT_EQ(published.payload_digest, first.payload_digest);

    std::array<std::byte, 4> copied{};
    ASSERT_EQ(
        runtime.copy_live_control_payload(kMailbox, 1, copied),
        rt::Status::ok);
    EXPECT_EQ(copied[0], std::byte{1});
    EXPECT_EQ(copied[3], std::byte{4});
    std::array<std::byte, 3> short_output{
        std::byte{7}, std::byte{7}, std::byte{7}};
    EXPECT_EQ(
        runtime.copy_live_control_payload(kMailbox, 1, short_output),
        rt::Status::capacity_exceeded);
    EXPECT_EQ(short_output[0], std::byte{7});

    const std::array second_payload{std::byte{5}, std::byte{6}};
    auto second = host_update(handle, 2, second_payload, 12);
    second.payload_digest ^= 1;
    ASSERT_EQ(
        runtime.stage_live_control_update(
            handle, second, second_payload, result),
        rt::Status::ok);
    EXPECT_EQ(result, rt::LiveControlAdmissionResult::invalid);
    second.payload_digest = rt::live_control_payload_digest(second_payload);
    ASSERT_EQ(
        runtime.stage_live_control_update(
            handle, second, second_payload, result),
        rt::Status::ok);
    EXPECT_EQ(result, rt::LiveControlAdmissionResult::accepted);

    auto one_over = host_update(handle, 3, second_payload, 13);
    ASSERT_EQ(
        runtime.stage_live_control_update(
            handle, one_over, second_payload, result),
        rt::Status::ok);
    EXPECT_EQ(result, rt::LiveControlAdmissionResult::full);

    rt::LiveControlMailboxInfo info;
    ASSERT_TRUE(runtime.live_control_mailbox_info(kMailbox, info));
    EXPECT_EQ(info.accepted, 2u);
    EXPECT_EQ(info.invalid, 1u);
    EXPECT_EQ(info.full, 1u);
    EXPECT_EQ(info.occupancy, 2u);
    EXPECT_EQ(info.next_mailbox_sequence, 3u);
    EXPECT_TRUE(info.admission_open);
}

TEST(LiveControlMailbox, OneAttemptContentionReturnsBusyWithoutPublication) {
    rt::Runtime runtime;
    configure_live_control(runtime, 1, 8);
    const auto handle = finalized_handle(runtime);
    const std::array payload{std::byte{0x61}};
    const auto update = host_update(handle, 1, payload);
    ASSERT_TRUE(
        rt::detail::RuntimeLiveControlTestAccess::claim(runtime, kMailbox));
    rt::LiveControlAdmissionResult result;
    ASSERT_EQ(
        runtime.stage_live_control_update(handle, update, payload, result),
        rt::Status::ok);
    EXPECT_EQ(result, rt::LiveControlAdmissionResult::busy);
    rt::LiveControlMailboxInfo info;
    ASSERT_TRUE(runtime.live_control_mailbox_info(kMailbox, info));
    EXPECT_EQ(info.busy, 1u);
    EXPECT_EQ(info.occupancy, 0u);
    rt::LiveControlUpdateRecord unpublished;
    EXPECT_FALSE(runtime.live_control_record_at(kMailbox, 1, unpublished));
    rt::detail::RuntimeLiveControlTestAccess::release(runtime, kMailbox);
    ASSERT_EQ(
        runtime.stage_live_control_update(handle, update, payload, result),
        rt::Status::ok);
    EXPECT_EQ(result, rt::LiveControlAdmissionResult::accepted);
}

TEST(LiveControlMailbox, ValidatesExactCompiledRateTargetsAndClosedPayloadKinds) {
    rt::Runtime runtime;
    rt::PhaseHandle phase;
    rt::RateDomainHandle domain;
    ASSERT_EQ(
        runtime.register_callback({"phase", &noop_callback, nullptr}, phase),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_rate_domain(
            {"control", 1'000, 1, 1'000, 100}, domain),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.bind_phase_to_rate_domain(phase, domain),
        rt::Status::ok);
    configure_live_control(runtime, 2, 8);
    const auto handle = finalized_handle(runtime);

    rt::ReferenceRelease release;
    ASSERT_TRUE(runtime.reference_release_at(0, release));
    const std::array payload{std::byte{0x44}};
    auto update = host_update(handle, 1, payload);
    update.target_kind = rt::LiveControlTargetKind::rate_release;
    update.target_frame_index = std::numeric_limits<std::uint64_t>::max();
    update.reference_release_index = 0;
    update.rate_domain_registration_index =
        static_cast<std::uint32_t>(release.domain_registration_index);
    update.phase_index = release.phase.index();
    update.rate_substep_ordinal = release.substep_ordinal;
    update.rate_release_sequence = release.domain_release_sequence;

    rt::LiveControlAdmissionResult result;
    ASSERT_EQ(
        runtime.stage_live_control_update(handle, update, payload, result),
        rt::Status::ok);
    EXPECT_EQ(result, rt::LiveControlAdmissionResult::accepted);

    auto contradictory = host_update(handle, 2, payload);
    contradictory.reference_release_index = 0;
    ASSERT_EQ(
        runtime.stage_live_control_update(
            handle, contradictory, payload, result),
        rt::Status::ok);
    EXPECT_EQ(result, rt::LiveControlAdmissionResult::invalid);

    auto out_of_plan = update;
    out_of_plan.producer_sequence = 2;
    out_of_plan.reference_release_index = 1;
    ASSERT_EQ(
        runtime.stage_live_control_update(
            handle, out_of_plan, payload, result),
        rt::Status::ok);
    EXPECT_EQ(result, rt::LiveControlAdmissionResult::invalid);

    const auto no_payload = std::span<const std::byte>{};
    auto missing = host_update(handle, 2, no_payload);
    missing.payload_digest = rt::live_control_payload_digest(no_payload);
    ASSERT_EQ(
        runtime.stage_live_control_update(
            handle, missing, no_payload, result),
        rt::Status::ok);
    EXPECT_EQ(result, rt::LiveControlAdmissionResult::invalid);
    auto nonempty_clear = host_update(handle, 2, payload);
    nonempty_clear.update_kind = rt::LiveControlUpdateKind::clear_fault;
    ASSERT_EQ(
        runtime.stage_live_control_update(
            handle, nonempty_clear, payload, result),
        rt::Status::ok);
    EXPECT_EQ(result, rt::LiveControlAdmissionResult::invalid);
    auto empty = host_update(handle, 2, no_payload);
    empty.update_kind = rt::LiveControlUpdateKind::clear_fault;
    empty.payload_digest = rt::live_control_payload_digest(no_payload);
    ASSERT_EQ(
        runtime.stage_live_control_update(handle, empty, no_payload, result),
        rt::Status::ok);
    EXPECT_EQ(result, rt::LiveControlAdmissionResult::accepted);
}

TEST(LiveControlMailbox, PastTargetsBecomeMissedAndStopClosesAdmission) {
    rt::Runtime runtime;
    configure_live_control(runtime, 2, 8);
    const auto handle = finalized_handle(runtime);
    const std::array payload{std::byte{0x31}};
    auto update = host_update(handle, 1, payload);
    rt::LiveControlAdmissionResult result;
    ASSERT_EQ(
        runtime.stage_live_control_update(handle, update, payload, result),
        rt::Status::ok);
    ASSERT_EQ(result, rt::LiveControlAdmissionResult::accepted);
    const auto before = metadata(runtime);

    ASSERT_EQ(runtime.start(), rt::Status::ok);
    rt::StepResult step;
    ASSERT_EQ(
        runtime.step(
            rt::HostFrameContext{
                11, std::chrono::nanoseconds{1'000}, std::nullopt},
            &step),
        rt::Status::ok);
    EXPECT_EQ(step.callbacks_executed, 0u);
    EXPECT_TRUE(runtime.live_control_record_at(kMailbox, 1, update));
    rt::LiveControlRecordStatusInfo record_status;
    ASSERT_TRUE(runtime.live_control_record_status(
        kMailbox, 1, record_status));
    EXPECT_EQ(record_status.status, rt::LiveControlRecordStatus::missed);
    const auto after = metadata(runtime);
    EXPECT_EQ(after.config_id, before.config_id);

    ASSERT_EQ(runtime.stop(), rt::Status::ok);
    auto stopped_update = host_update(handle, 2, payload, 12);
    ASSERT_EQ(
        runtime.stage_live_control_update(
            handle, stopped_update, payload, result),
        rt::Status::ok);
    EXPECT_EQ(result, rt::LiveControlAdmissionResult::stopped);
    rt::LiveControlMailboxInfo info;
    ASSERT_TRUE(runtime.live_control_mailbox_info(kMailbox, info));
    EXPECT_FALSE(info.admission_open);
    EXPECT_EQ(info.stopped, 1u);
    EXPECT_EQ(info.occupancy, 0u);
}

TEST(LiveControlMailbox, RuntimeCallbacksCannotUseTheProducerAdmissionApi) {
    rt::Runtime runtime;
    ReentrantAdmissionProbe probe;
    probe.runtime = &runtime;
    ASSERT_EQ(
        runtime.register_callback(
            {"reentrant", &reentrant_admission_callback, &probe}),
        rt::Status::ok);
    configure_live_control(runtime, 1, 8);
    probe.handle = finalized_handle(runtime);
    probe.update = host_update(probe.handle, 1, probe.payload);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(
        runtime.step(
            rt::HostFrameContext{
                1, std::chrono::nanoseconds{1'000}, std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(probe.status, rt::Status::invalid_state);
    rt::LiveControlMailboxInfo info;
    ASSERT_TRUE(runtime.live_control_mailbox_info(kMailbox, info));
    EXPECT_EQ(info.occupancy, 0u);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(LiveControlMailbox, StopRaceEitherCompletesOrRejectsOneWholeRecord) {
    rt::Runtime runtime;
    configure_live_control(
        runtime,
        1,
        rt::live_control_payload_bytes_limit);
    const auto handle = finalized_handle(runtime);
    std::vector<std::byte> payload(
        rt::live_control_payload_bytes_limit,
        std::byte{0x5a});
    const auto update = host_update(handle, 1, payload);
    rt::Status stage_status = rt::Status::internal_error;
    rt::LiveControlAdmissionResult result =
        rt::LiveControlAdmissionResult::invalid;
    std::barrier start(2);
    std::thread producer_thread([&] {
        start.arrive_and_wait();
        stage_status = runtime.stage_live_control_update(
            handle, update, payload, result);
    });
    start.arrive_and_wait();
    const auto first_stop_status = runtime.stop();
    producer_thread.join();

    EXPECT_TRUE(
        first_stop_status == rt::Status::ok ||
        first_stop_status == rt::Status::invalid_state);
    if (first_stop_status == rt::Status::invalid_state) {
        ASSERT_EQ(runtime.stop(), rt::Status::ok);
    }
    EXPECT_EQ(stage_status, rt::Status::ok);
    EXPECT_TRUE(
        result == rt::LiveControlAdmissionResult::accepted ||
        result == rt::LiveControlAdmissionResult::stopped);
    rt::LiveControlMailboxInfo info;
    ASSERT_TRUE(runtime.live_control_mailbox_info(kMailbox, info));
    EXPECT_FALSE(info.admission_open);
    EXPECT_EQ(info.occupancy, 0u);
    rt::LiveControlUpdateRecord record;
    if (runtime.live_control_record_at(kMailbox, 1, record)) {
        std::vector<std::byte> copied(payload.size());
        ASSERT_EQ(
            runtime.copy_live_control_payload(kMailbox, 1, copied),
            rt::Status::ok);
        EXPECT_EQ(copied, payload);
        rt::LiveControlRecordStatusInfo status;
        ASSERT_TRUE(runtime.live_control_record_status(
            kMailbox, 1, status));
        EXPECT_EQ(status.status, rt::LiveControlRecordStatus::stopped);
    }
}

TEST(LiveControlMailbox, HandlesAndStorageAreIsolatedPerRuntime) {
    rt::Runtime first_runtime;
    rt::Runtime second_runtime;
    configure_live_control(first_runtime, 1, 8);
    configure_live_control(second_runtime, 1, 8);
    const auto first_handle = finalized_handle(first_runtime);
    const auto second_handle = finalized_handle(second_runtime);
    ASSERT_NE(first_handle.runtime_id, second_handle.runtime_id);

    const std::array payload{std::byte{0x72}};
    const auto first_update = host_update(first_handle, 1, payload);
    rt::LiveControlAdmissionResult result;
    ASSERT_EQ(
        second_runtime.stage_live_control_update(
            first_handle, first_update, payload, result),
        rt::Status::ok);
    EXPECT_EQ(result, rt::LiveControlAdmissionResult::stale);
    rt::LiveControlMailboxInfo info;
    ASSERT_TRUE(second_runtime.live_control_mailbox_info(kMailbox, info));
    EXPECT_EQ(info.occupancy, 0u);
    EXPECT_EQ(info.stale, 0u);

    const auto second_update = host_update(second_handle, 1, payload);
    ASSERT_EQ(
        second_runtime.stage_live_control_update(
            second_handle, second_update, payload, result),
        rt::Status::ok);
    EXPECT_EQ(result, rt::LiveControlAdmissionResult::accepted);
    ASSERT_TRUE(second_runtime.live_control_mailbox_info(kMailbox, info));
    EXPECT_EQ(info.occupancy, 1u);
    ASSERT_TRUE(first_runtime.live_control_mailbox_info(kMailbox, info));
    EXPECT_EQ(info.occupancy, 0u);
}

TEST(LiveControlMailbox, ConcurrentProducersPublishOnlyCompleteDenseRecords) {
    constexpr std::size_t producer_count = 8;
    rt::Runtime runtime;
    ASSERT_EQ(
        runtime.set_live_control_policy(
            policy(1, producer_count, producer_count, 8)),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_live_control_mailbox(
            mailbox(kMailbox, producer_count, 8)),
        rt::Status::ok);
    for (std::size_t index = 0; index < producer_count; ++index) {
        ASSERT_EQ(
            runtime.register_live_control_producer(
                producer(kMailbox, 1'000 + index)),
            rt::Status::ok);
    }
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);

    std::array<rt::LiveControlProducerHandle, producer_count> handles{};
    std::array<rt::LiveControlUpdateRecord, producer_count> updates{};
    std::array<rt::Status, producer_count> statuses{};
    std::array<rt::LiveControlAdmissionResult, producer_count> results{};
    std::array<std::array<std::byte, 1>, producer_count> payloads{};
    for (std::size_t index = 0; index < producer_count; ++index) {
        ASSERT_EQ(
            runtime.live_control_producer_handle(
                kMailbox, 1'000 + index, handles[index]),
            rt::Status::ok);
        payloads[index][0] = static_cast<std::byte>(index + 1);
        updates[index] = host_update(handles[index], 1, payloads[index]);
    }

    std::barrier start(static_cast<std::ptrdiff_t>(producer_count));
    std::array<std::thread, producer_count> threads;
    for (std::size_t index = 0; index < producer_count; ++index) {
        threads[index] = std::thread([&, index] {
            start.arrive_and_wait();
            statuses[index] = runtime.stage_live_control_update(
                handles[index], updates[index], payloads[index], results[index]);
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    std::size_t busy = 0;
    for (std::size_t index = 0; index < producer_count; ++index) {
        EXPECT_EQ(statuses[index], rt::Status::ok);
        if (results[index] == rt::LiveControlAdmissionResult::busy) {
            ++busy;
            ASSERT_EQ(
                runtime.stage_live_control_update(
                    handles[index],
                    updates[index],
                    payloads[index],
                    results[index]),
                rt::Status::ok);
        }
        EXPECT_EQ(results[index], rt::LiveControlAdmissionResult::accepted);
    }

    rt::LiveControlMailboxInfo info;
    ASSERT_TRUE(runtime.live_control_mailbox_info(kMailbox, info));
    EXPECT_EQ(info.accepted, producer_count);
    EXPECT_EQ(info.busy, busy);
    EXPECT_EQ(info.occupancy, producer_count);
    for (std::uint64_t sequence = 1;
         sequence <= producer_count;
         ++sequence) {
        rt::LiveControlUpdateRecord record;
        ASSERT_TRUE(
            runtime.live_control_record_at(kMailbox, sequence, record));
        EXPECT_EQ(record.mailbox_sequence, sequence);
        std::array<std::byte, 1> copied{};
        EXPECT_EQ(
            runtime.copy_live_control_payload(
                kMailbox, sequence, copied),
            rt::Status::ok);
        EXPECT_EQ(
            rt::live_control_payload_digest(copied),
            record.payload_digest);
    }
}

TEST(LiveControlMailbox, BoundaryPublishesCanonicalReplacementAndReusesSlots) {
    rt::Runtime runtime;
    GenerationProbe probe;
    ASSERT_EQ(
        runtime.register_callback(
            {"capture", &capture_generation_callback, &probe}),
        rt::Status::ok);
    configure_live_control(runtime, 3, 8);
    const auto handle = finalized_handle(runtime);
    const std::array first_payload{std::byte{0x11}};
    const std::array second_payload{std::byte{0x22}};
    const std::array third_payload{std::byte{0x33}};
    auto first = host_update(handle, 1, first_payload, 7);
    auto second = host_update(handle, 2, second_payload, 7);
    second.update_kind = rt::LiveControlUpdateKind::controller_parameters;
    auto third = host_update(handle, 3, third_payload, 7);
    rt::LiveControlAdmissionResult result;
    ASSERT_EQ(
        runtime.stage_live_control_update(
            handle, first, first_payload, result),
        rt::Status::ok);
    ASSERT_EQ(result, rt::LiveControlAdmissionResult::accepted);
    ASSERT_EQ(
        runtime.stage_live_control_update(
            handle, second, second_payload, result),
        rt::Status::ok);
    ASSERT_EQ(result, rt::LiveControlAdmissionResult::accepted);
    ASSERT_EQ(
        runtime.stage_live_control_update(
            handle, third, third_payload, result),
        rt::Status::ok);
    ASSERT_EQ(result, rt::LiveControlAdmissionResult::accepted);

    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(
        runtime.step(rt::HostFrameContext{
            7, std::chrono::nanoseconds{1'000}, std::nullopt}),
        rt::Status::ok);
    ASSERT_NE(probe.generation_identity, 0u);
    EXPECT_EQ(probe.generation_identity, 0xd2ccc254bb8102daull);
    EXPECT_EQ(probe.target.kind, rt::LiveControlTargetKind::host_frame);
    EXPECT_EQ(probe.target.frame_index, 7u);
    ASSERT_EQ(probe.record_count, 2u);
    EXPECT_EQ(probe.mailbox_sequences[0], 2u);
    EXPECT_EQ(probe.mailbox_sequences[1], 3u);
    EXPECT_EQ(
        probe.update_kinds[0],
        rt::LiveControlUpdateKind::controller_parameters);
    EXPECT_EQ(
        probe.update_kinds[1],
        rt::LiveControlUpdateKind::scenario_parameters);
    EXPECT_EQ(probe.first_payload_bytes[0], std::byte{0x22});
    EXPECT_EQ(probe.first_payload_bytes[1], std::byte{0x33});

    rt::LiveControlCommitInfo commit;
    ASSERT_TRUE(runtime.live_control_commit_info(commit));
    EXPECT_EQ(commit.generation_identity, probe.generation_identity);
    EXPECT_EQ(commit.survivor_count, 2u);
    EXPECT_EQ(commit.committed, 2u);
    EXPECT_EQ(commit.replaced, 1u);
    EXPECT_EQ(commit.staged_occupancy, 0u);
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
        rt::LiveControlRecordStatusInfo status;
        ASSERT_TRUE(runtime.live_control_record_status(
            kMailbox, sequence, status));
        EXPECT_EQ(
            status.status,
            sequence == 1
                ? rt::LiveControlRecordStatus::replaced
                : rt::LiveControlRecordStatus::committed);
        EXPECT_EQ(status.generation_identity, commit.generation_identity);
    }

    const std::array fourth_payload{std::byte{0x44}};
    const auto fourth = host_update(handle, 4, fourth_payload, 8);
    ASSERT_EQ(
        runtime.stage_live_control_update(
            handle, fourth, fourth_payload, result),
        rt::Status::ok);
    ASSERT_EQ(result, rt::LiveControlAdmissionResult::accepted);
    rt::LiveControlRecordStatusInfo reclaimed;
    reclaimed.runtime_id = 0xfeed;
    EXPECT_FALSE(runtime.live_control_record_status(
        kMailbox, 1, reclaimed));
    EXPECT_EQ(reclaimed.runtime_id, 0xfeedu);
    ASSERT_EQ(
        runtime.step(rt::HostFrameContext{
            8, std::chrono::nanoseconds{1'000}, std::nullopt}),
        rt::Status::ok);
    ASSERT_EQ(probe.record_count, 1u);
    EXPECT_EQ(probe.mailbox_sequences[0], 4u);
    const auto fourth_generation = probe.generation_identity;
    ASSERT_EQ(
        runtime.step(rt::HostFrameContext{
            9, std::chrono::nanoseconds{1'000}, std::nullopt}),
        rt::Status::ok);
    EXPECT_EQ(probe.generation_identity, fourth_generation);
    EXPECT_EQ(probe.target.frame_index, 8u);

    const std::array late_payload{std::byte{0x55}};
    const auto late = host_update(handle, 5, late_payload, 8);
    ASSERT_EQ(
        runtime.stage_live_control_update(
            handle, late, late_payload, result),
        rt::Status::ok);
    EXPECT_EQ(result, rt::LiveControlAdmissionResult::missed);
    rt::LiveControlRecordStatusInfo late_status;
    ASSERT_TRUE(runtime.live_control_record_status(
        kMailbox, 5, late_status));
    EXPECT_EQ(late_status.status, rt::LiveControlRecordStatus::missed);
    ASSERT_TRUE(runtime.live_control_commit_info(commit));
    EXPECT_EQ(commit.missed, 1u);
    EXPECT_EQ(commit.staged_occupancy, 0u);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(LiveControlMailbox, BoundaryOrderIgnoresRegistrationAndArrivalOrder) {
    rt::Runtime runtime;
    GenerationProbe probe;
    ASSERT_EQ(
        runtime.register_callback(
            {"capture", &capture_generation_callback, &probe}),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.set_live_control_policy(policy(2, 2, 2, 8)),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_live_control_mailbox(mailbox(kMailbox + 1, 1, 8)),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_live_control_mailbox(mailbox(kMailbox, 1, 8)),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_live_control_producer(
            producer(kMailbox + 1, kProducer + 1)),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.register_live_control_producer(
            producer(kMailbox, kProducer)),
        rt::Status::ok);
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    rt::LiveControlProducerHandle high_handle;
    rt::LiveControlProducerHandle low_handle;
    ASSERT_EQ(
        runtime.live_control_producer_handle(
            kMailbox + 1, kProducer + 1, high_handle),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.live_control_producer_handle(
            kMailbox, kProducer, low_handle),
        rt::Status::ok);
    const std::array high_payload{std::byte{0x72}};
    const std::array low_payload{std::byte{0x71}};
    const auto high = host_update(high_handle, 1, high_payload, 7);
    const auto low = host_update(low_handle, 1, low_payload, 7);
    rt::LiveControlAdmissionResult admission;
    ASSERT_EQ(
        runtime.stage_live_control_update(
            high_handle, high, high_payload, admission),
        rt::Status::ok);
    ASSERT_EQ(admission, rt::LiveControlAdmissionResult::accepted);
    ASSERT_EQ(
        runtime.stage_live_control_update(
            low_handle, low, low_payload, admission),
        rt::Status::ok);
    ASSERT_EQ(admission, rt::LiveControlAdmissionResult::accepted);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(
        runtime.step({7, std::chrono::nanoseconds{1'000}}),
        rt::Status::ok);
    ASSERT_EQ(probe.record_count, 2u);
    EXPECT_EQ(probe.mailbox_identities[0], kMailbox);
    EXPECT_EQ(probe.mailbox_identities[1], kMailbox + 1);
    EXPECT_EQ(probe.first_payload_bytes[0], std::byte{0x71});
    EXPECT_EQ(probe.first_payload_bytes[1], std::byte{0x72});
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(LiveControlMailbox, ActiveRateReleasePublishesBeforeItsCallback) {
    ManualClock clock;
    rt::Runtime runtime(clock);
    GenerationProbe probe;
    ASSERT_EQ(runtime.set_rate_execution_policy({4}), rt::Status::ok);
    rt::PhaseHandle phase;
    ASSERT_EQ(
        runtime.register_callback(
            {"rate-capture", &capture_generation_callback, &probe}, phase),
        rt::Status::ok);
    rt::RateDomainRegistration domain_registration;
    domain_registration.name = "control-rate";
    domain_registration.period_ns = 100;
    domain_registration.relative_deadline_ns = 100;
    domain_registration.budget_wcet_ns = 10;
    domain_registration.late_action = rt::RateLateAction::fail;
    rt::RateDomainHandle domain;
    ASSERT_EQ(
        runtime.register_rate_domain(domain_registration, domain),
        rt::Status::ok);
    ASSERT_EQ(
        runtime.bind_phase_to_rate_domain(phase, domain),
        rt::Status::ok);
    configure_live_control(runtime, 1, 8);
    const auto handle = finalized_handle(runtime);
    rt::ReferenceRelease release;
    ASSERT_TRUE(runtime.reference_release_at(0, release));

    const std::array payload{std::byte{0x6a}};
    auto update = host_update(handle, 1, payload);
    update.target_kind = rt::LiveControlTargetKind::rate_release;
    update.target_frame_index = std::numeric_limits<std::uint64_t>::max();
    update.reference_release_index = 0;
    update.rate_domain_registration_index =
        static_cast<std::uint32_t>(release.domain_registration_index);
    update.phase_index = release.phase.index();
    update.rate_substep_ordinal = release.substep_ordinal;
    update.rate_release_sequence = release.domain_release_sequence;
    rt::LiveControlAdmissionResult admission;
    ASSERT_EQ(
        runtime.stage_live_control_update(
            handle, update, payload, admission),
        rt::Status::ok);
    ASSERT_EQ(admission, rt::LiveControlAdmissionResult::accepted);

    ASSERT_EQ(runtime.start(), rt::Status::ok);
    rt::StepResult result;
    ASSERT_EQ(
        runtime.step(
            {42, std::chrono::nanoseconds{100}, std::nullopt, 1'000},
            &result),
        rt::Status::ok);
    EXPECT_EQ(result.callbacks_executed, 1u);
    EXPECT_EQ(probe.calls, 1u);
    EXPECT_NE(probe.generation_identity, 0u);
    EXPECT_EQ(probe.target.kind, rt::LiveControlTargetKind::rate_release);
    EXPECT_EQ(probe.target.reference_release_index, 0u);
    EXPECT_EQ(
        probe.target.rate_release_sequence,
        release.domain_release_sequence);
    ASSERT_EQ(probe.record_count, 1u);
    EXPECT_EQ(probe.first_payload_bytes[0], std::byte{0x6a});
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(LiveControlMailbox, ExhaustionPrecedesWrapAndDoesNotPublish) {
    rt::Runtime runtime;
    constexpr auto last_sequence =
        std::numeric_limits<std::uint64_t>::max() - 1;
    configure_live_control(runtime, 2, 8, last_sequence);
    const auto handle = finalized_handle(runtime);
    const std::array payload{std::byte{0x11}};
    auto update = host_update(handle, last_sequence, payload);
    rt::LiveControlAdmissionResult result;
    ASSERT_EQ(
        runtime.stage_live_control_update(handle, update, payload, result),
        rt::Status::ok);
    EXPECT_EQ(result, rt::LiveControlAdmissionResult::accepted);
    update.producer_sequence = std::numeric_limits<std::uint64_t>::max();
    ASSERT_EQ(
        runtime.stage_live_control_update(handle, update, payload, result),
        rt::Status::ok);
    EXPECT_EQ(result, rt::LiveControlAdmissionResult::exhausted);
    rt::LiveControlMailboxInfo info;
    ASSERT_TRUE(runtime.live_control_mailbox_info(kMailbox, info));
    EXPECT_EQ(info.accepted, 1u);
    EXPECT_EQ(info.exhausted, 1u);
    EXPECT_EQ(info.occupancy, 1u);
}

TEST(LiveControlMailbox, FrozenPolicyChangesIdentityButArrivalsDoNot) {
    rt::Runtime first;
    configure_live_control(first, 1, 8);
    const auto handle = finalized_handle(first);
    const auto before = compatibility_ids(first);
    const auto checkpoint_before = checkpoint_artifact(first);
    const std::array payload{std::byte{0x21}};
    const auto update = host_update(handle, 1, payload);
    rt::LiveControlAdmissionResult result;
    ASSERT_EQ(
        first.stage_live_control_update(handle, update, payload, result),
        rt::Status::ok);
    ASSERT_EQ(result, rt::LiveControlAdmissionResult::accepted);
    const auto after = compatibility_ids(first);
    const auto checkpoint_after = checkpoint_artifact(first);
    EXPECT_EQ(after.config, before.config);
    EXPECT_EQ(after.graph, before.graph);
    EXPECT_EQ(after.replay, before.replay);
    EXPECT_EQ(checkpoint_after, checkpoint_before);

    rt::Runtime different_policy;
    auto changed = policy(1, 1, 1, 8);
    ++changed.policy_identity;
    ASSERT_EQ(
        different_policy.set_live_control_policy(changed),
        rt::Status::ok);
    ASSERT_EQ(
        different_policy.register_live_control_mailbox(
            mailbox(kMailbox, 1, 8)),
        rt::Status::ok);
    ASSERT_EQ(
        different_policy.register_live_control_producer(producer()),
        rt::Status::ok);
    ASSERT_EQ(different_policy.finalize(), rt::Status::ok);
    const auto different = compatibility_ids(different_policy);
    EXPECT_NE(different.config, before.config);
    EXPECT_NE(different.graph, before.graph);
    EXPECT_NE(different.replay, before.replay);

    const auto ids_for_declaration_order = [](
        bool reverse,
        std::uint64_t first_sequence) {
        rt::Runtime runtime;
        EXPECT_EQ(
            runtime.set_live_control_policy(policy(2, 2, 2, 8)),
            rt::Status::ok);
        const auto first_mailbox = mailbox(kMailbox, 1, 8);
        const auto second_mailbox = mailbox(kMailbox + 1, 1, 8);
        const auto first_producer = producer(
            kMailbox,
            kProducer,
            first_sequence);
        const auto second_producer = producer(
            kMailbox + 1,
            kProducer + 1,
            1);
        if (reverse) {
            EXPECT_EQ(
                runtime.register_live_control_mailbox(second_mailbox),
                rt::Status::ok);
            EXPECT_EQ(
                runtime.register_live_control_mailbox(first_mailbox),
                rt::Status::ok);
            EXPECT_EQ(
                runtime.register_live_control_producer(second_producer),
                rt::Status::ok);
            EXPECT_EQ(
                runtime.register_live_control_producer(first_producer),
                rt::Status::ok);
        } else {
            EXPECT_EQ(
                runtime.register_live_control_mailbox(first_mailbox),
                rt::Status::ok);
            EXPECT_EQ(
                runtime.register_live_control_mailbox(second_mailbox),
                rt::Status::ok);
            EXPECT_EQ(
                runtime.register_live_control_producer(first_producer),
                rt::Status::ok);
            EXPECT_EQ(
                runtime.register_live_control_producer(second_producer),
                rt::Status::ok);
        }
        EXPECT_EQ(runtime.finalize(), rt::Status::ok);
        return compatibility_ids(runtime);
    };
    const auto declaration_ids = ids_for_declaration_order(false, 1);
    const auto reordered_ids = ids_for_declaration_order(true, 1);
    EXPECT_EQ(reordered_ids.config, declaration_ids.config);
    EXPECT_EQ(reordered_ids.graph, declaration_ids.graph);
    EXPECT_EQ(reordered_ids.replay, declaration_ids.replay);
    const auto changed_sequence_ids =
        ids_for_declaration_order(false, 2);
    EXPECT_NE(changed_sequence_ids.config, declaration_ids.config);
    EXPECT_NE(changed_sequence_ids.graph, declaration_ids.graph);
    EXPECT_NE(changed_sequence_ids.replay, declaration_ids.replay);
}

TEST(LiveControlMailbox, ClosureSettlesSuccessWithCanonicalActions) {
    rt::Runtime runtime;
    GenerationProbe probe;
    ASSERT_EQ(
        runtime.register_callback(
            {"capture", &capture_generation_callback, &probe}),
        rt::Status::ok);
    configure_live_control(runtime, 1, 8);
    ASSERT_EQ(
        runtime.set_live_control_closure_policy(closure_policy()),
        rt::Status::ok);
    const auto handle = finalized_handle(runtime);
    EXPECT_TRUE(runtime.live_control_closure_enabled());
    rt::MemoryPlan plan;
    ASSERT_TRUE(runtime.memory_plan(plan));
    EXPECT_EQ(plan.live_control_action_capacity, 128u);
    EXPECT_GT(plan.live_control_action_storage_bytes, 0u);
    EXPECT_GT(plan.live_control_closure_control_bytes,
              plan.live_control_action_storage_bytes);
    EXPECT_GT(plan.live_control_checkpoint_state_bytes, 0u);

    const std::array payload{std::byte{0x41}};
    const auto update = host_update(handle, 1, payload, 7);
    rt::LiveControlAdmissionResult admission;
    ASSERT_EQ(
        runtime.stage_live_control_update(
            handle, update, payload, admission),
        rt::Status::ok);
    ASSERT_EQ(admission, rt::LiveControlAdmissionResult::accepted);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    ASSERT_EQ(
        runtime.step({7, std::chrono::nanoseconds{1'000}}),
        rt::Status::ok);

    rt::LiveControlRecordStatusInfo status;
    ASSERT_TRUE(runtime.live_control_record_status(kMailbox, 1, status));
    EXPECT_EQ(status.status, rt::LiveControlRecordStatus::committed);
    EXPECT_EQ(status.generation_identity, probe.generation_identity);

    std::array<rt::LiveControlActionRecord, 8> actions{};
    rt::LiveControlActionCursor cursor;
    rt::LiveControlActionReadResult read;
    ASSERT_EQ(
        runtime.read_live_control_actions(cursor, actions, read),
        rt::Status::ok);
    ASSERT_EQ(read.records_read, 3u);
    EXPECT_EQ(actions[0].action, rt::LiveControlActionId::admission);
    EXPECT_EQ(
        actions[1].action,
        rt::LiveControlActionId::provisional_publication);
    EXPECT_EQ(actions[2].action, rt::LiveControlActionId::committed);
    EXPECT_EQ(actions[2].record_status,
              rt::LiveControlRecordStatus::committed);
    EXPECT_EQ(actions[2].payload_digest,
              rt::live_control_payload_digest(payload));
    EXPECT_EQ(read.lost_records, 0u);
    EXPECT_EQ(read.metadata.records_dropped, 0u);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(LiveControlMailbox, CallbackFailureRestoresPriorGenerationAndRollsBack) {
    rt::Runtime runtime;
    ASSERT_EQ(
        runtime.register_callback({"fail", &failing_callback, nullptr}),
        rt::Status::ok);
    configure_live_control(runtime, 1, 8);
    ASSERT_EQ(
        runtime.set_live_control_closure_policy(closure_policy()),
        rt::Status::ok);
    const auto handle = finalized_handle(runtime);
    const std::array payload{std::byte{0x51}};
    const auto update = host_update(handle, 1, payload, 7);
    rt::LiveControlAdmissionResult admission;
    ASSERT_EQ(
        runtime.stage_live_control_update(
            handle, update, payload, admission),
        rt::Status::ok);
    ASSERT_EQ(admission, rt::LiveControlAdmissionResult::accepted);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    EXPECT_EQ(
        runtime.step({7, std::chrono::nanoseconds{1'000}}),
        rt::Status::callback_failed);

    rt::LiveControlCommitInfo commit;
    ASSERT_TRUE(runtime.live_control_commit_info(commit));
    EXPECT_EQ(commit.generation_identity, 0u);
    EXPECT_EQ(commit.survivor_count, 0u);
    EXPECT_EQ(commit.committed, 0u);
    EXPECT_EQ(commit.staged_occupancy, 0u);
    rt::LiveControlRecordStatusInfo status;
    ASSERT_TRUE(runtime.live_control_record_status(kMailbox, 1, status));
    EXPECT_EQ(status.status, rt::LiveControlRecordStatus::rolled_back);

    std::array<rt::LiveControlActionRecord, 8> actions{};
    rt::LiveControlActionCursor cursor;
    rt::LiveControlActionReadResult read;
    ASSERT_EQ(
        runtime.read_live_control_actions(cursor, actions, read),
        rt::Status::ok);
    ASSERT_EQ(read.records_read, 3u);
    EXPECT_EQ(actions[2].action, rt::LiveControlActionId::rolled_back);
    EXPECT_EQ(actions[2].result, rt::LiveControlActionResult::rolled_back);
    EXPECT_EQ(
        actions[2].terminal_status,
        static_cast<std::int32_t>(rt::Status::callback_failed));
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(LiveControlMailbox, CheckpointRoundTripRetiresHandlesWithoutPartialRestore) {
    rt::Runtime runtime;
    configure_live_control(runtime, 2, 8);
    ASSERT_EQ(
        runtime.set_live_control_closure_policy(closure_policy()),
        rt::Status::ok);
    const auto old_handle = finalized_handle(runtime);
    const std::array first_payload{std::byte{0x61}, std::byte{0x62}};
    const auto first = host_update(old_handle, 1, first_payload, 7);
    rt::LiveControlAdmissionResult admission;
    ASSERT_EQ(
        runtime.stage_live_control_update(
            old_handle, first, first_payload, admission),
        rt::Status::ok);
    ASSERT_EQ(admission, rt::LiveControlAdmissionResult::accepted);
    const auto checkpoint = checkpoint_artifact(runtime);

    const std::array second_payload{std::byte{0x71}};
    const auto second = host_update(old_handle, 2, second_payload, 8);
    ASSERT_EQ(
        runtime.stage_live_control_update(
            old_handle, second, second_payload, admission),
        rt::Status::ok);
    ASSERT_EQ(admission, rt::LiveControlAdmissionResult::accepted);
    auto corrupt = checkpoint;
    corrupt.back() ^= std::byte{1};
    EXPECT_EQ(
        runtime.restore_checkpoint(corrupt),
        rt::Status::invalid_artifact);
    rt::LiveControlUpdateRecord retained;
    EXPECT_TRUE(runtime.live_control_record_at(kMailbox, 2, retained));

    ASSERT_EQ(runtime.restore_checkpoint(checkpoint), rt::Status::ok);
    EXPECT_FALSE(runtime.live_control_record_at(kMailbox, 2, retained));
    ASSERT_TRUE(runtime.live_control_record_at(kMailbox, 1, retained));
    EXPECT_NE(retained.configuration_generation,
              old_handle.configuration_generation);
    std::array<std::byte, 2> copied{};
    ASSERT_EQ(
        runtime.copy_live_control_payload(kMailbox, 1, copied),
        rt::Status::ok);
    EXPECT_EQ(copied, first_payload);

    auto stale_update = host_update(old_handle, 2, second_payload, 8);
    ASSERT_EQ(
        runtime.stage_live_control_update(
            old_handle, stale_update, second_payload, admission),
        rt::Status::ok);
    EXPECT_EQ(admission, rt::LiveControlAdmissionResult::stale);
    rt::LiveControlProducerHandle current_handle;
    ASSERT_EQ(
        runtime.live_control_producer_handle(
            kMailbox, kProducer, current_handle),
        rt::Status::ok);
    const auto resumed = host_update(current_handle, 2, second_payload, 8);
    ASSERT_EQ(
        runtime.stage_live_control_update(
            current_handle, resumed, second_payload, admission),
        rt::Status::ok);
    EXPECT_EQ(admission, rt::LiveControlAdmissionResult::accepted);
}

TEST(LiveControlMailbox, OrdinaryReplayInjectsExactRetainedGeneration) {
    rt::Runtime runtime;
    GenerationProbe probe;
    ASSERT_EQ(
        runtime.register_callback(
            {"capture", &capture_generation_callback, &probe}),
        rt::Status::ok);
    configure_live_control(runtime, 1, 8);
    ASSERT_EQ(
        runtime.set_live_control_closure_policy(closure_policy(true)),
        rt::Status::ok);
    const auto handle = finalized_handle(runtime);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    const auto checkpoint = checkpoint_artifact(runtime);

    // The producer admission deliberately occurs after the checkpoint.
    // Replay must reconstruct it from the validated action/journal range and
    // must not depend on the external producer running a second time.
    const std::array payload{std::byte{0x7a}};
    const auto update = host_update(handle, 1, payload, 7);
    rt::LiveControlAdmissionResult admission;
    ASSERT_EQ(
        runtime.stage_live_control_update(
            handle, update, payload, admission),
        rt::Status::ok);
    ASSERT_EQ(admission, rt::LiveControlAdmissionResult::accepted);

    const std::array<rt::ReplayInputRecord, 1> inputs{
        rt::ReplayInputRecord{
            {7, std::chrono::nanoseconds{1'000}}, 1, {}}};
    std::vector<std::byte> input_log(4'096);
    rt::ArtifactWriteResult input_write;
    ASSERT_EQ(
        runtime.write_input_log(inputs, input_log, input_write),
        rt::Status::ok);
    input_log.resize(input_write.bytes_written);

    ASSERT_EQ(
        runtime.step({7, std::chrono::nanoseconds{1'000}}),
        rt::Status::ok);
    const auto expected_generation = probe.generation_identity;
    ASSERT_NE(expected_generation, 0u);

    std::array<std::byte, 1> short_output{std::byte{0x5a}};
    rt::ArtifactWriteResult short_write;
    EXPECT_EQ(
        runtime.write_live_control_replay_artifact(
            checkpoint,
            input_log,
            rt::LiveControlNestedArtifactKind::input_log,
            short_output,
            short_write),
        rt::Status::capacity_exceeded);
    EXPECT_EQ(short_output.front(), std::byte{0x5a});
    ASSERT_GT(short_write.required_bytes, short_output.size());
    std::vector<std::byte> artifact(short_write.required_bytes);
    rt::ArtifactWriteResult write;
    ASSERT_EQ(
        runtime.write_live_control_replay_artifact(
            checkpoint,
            input_log,
            rt::LiveControlNestedArtifactKind::input_log,
            artifact,
            write),
        rt::Status::ok);
    artifact.resize(write.bytes_written);

    rt::LiveControlReplayMetadata inspected;
    ASSERT_EQ(
        rt::inspect_live_control_replay_artifact(artifact, inspected),
        rt::Status::ok);
    EXPECT_EQ(inspected.nested_kind,
              rt::LiveControlNestedArtifactKind::input_log);
    EXPECT_EQ(inspected.retained_generation_count, 1u);
    EXPECT_EQ(inspected.retained_record_count, 1u);
    const auto artifact_checksum = inspected.artifact_checksum;
    auto corrupt = artifact;
    corrupt[corrupt.size() / 2] ^= std::byte{1};
    EXPECT_EQ(
        rt::inspect_live_control_replay_artifact(corrupt, inspected),
        rt::Status::invalid_artifact);

    rt::LiveControlReplayResult replay;
    ASSERT_EQ(
        runtime.replay_live_control(
            artifact, &noop_replay_input, nullptr, &replay),
        rt::Status::ok);
    EXPECT_EQ(replay.frames_replayed, 1u);
    EXPECT_EQ(replay.generations_compared, 1u);
    EXPECT_EQ(replay.mismatch_status, rt::Status::ok);
    EXPECT_EQ(probe.generation_identity, expected_generation);
    rt::LiveControlRecordStatusInfo status;
    ASSERT_TRUE(runtime.live_control_record_status(kMailbox, 1, status));
    EXPECT_EQ(status.status, rt::LiveControlRecordStatus::committed);
    std::array<rt::LiveControlActionRecord, 8> replay_actions{};
    rt::LiveControlActionCursor replay_cursor;
    rt::LiveControlActionReadResult replay_read;
    ASSERT_EQ(
        runtime.read_live_control_actions(
            replay_cursor, replay_actions, replay_read),
        rt::Status::ok);
    ASSERT_EQ(replay_read.records_read, 1u);
    EXPECT_EQ(replay_read.lost_records, 4u);
    EXPECT_EQ(
        replay_actions[0].action,
        rt::LiveControlActionId::replay_verified);
    EXPECT_EQ(
        replay_actions[0].replay_correlation,
        artifact_checksum);
    rt::LiveControlProducerHandle resumed_handle;
    ASSERT_EQ(
        runtime.live_control_producer_handle(
            kMailbox, kProducer, resumed_handle),
        rt::Status::ok);
    const auto resumed = host_update(resumed_handle, 2, payload, 8);
    ASSERT_EQ(
        runtime.stage_live_control_update(
            resumed_handle, resumed, payload, admission),
        rt::Status::ok);
    EXPECT_EQ(admission, rt::LiveControlAdmissionResult::accepted);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}

TEST(LiveControlMailbox, ReplayMatchesRecordedRollbackBeforeReturningFailure) {
    rt::Runtime runtime;
    GenerationProbe probe;
    ASSERT_EQ(
        runtime.register_callback(
            {"capture-fail", &capture_generation_then_fail_callback, &probe}),
        rt::Status::ok);
    configure_live_control(runtime, 1, 8);
    ASSERT_EQ(
        runtime.set_live_control_closure_policy(closure_policy(true)),
        rt::Status::ok);
    const auto handle = finalized_handle(runtime);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    const auto checkpoint = checkpoint_artifact(runtime);

    const std::array payload{std::byte{0x3a}};
    const auto update = host_update(handle, 1, payload, 11);
    rt::LiveControlAdmissionResult admission;
    ASSERT_EQ(
        runtime.stage_live_control_update(
            handle, update, payload, admission),
        rt::Status::ok);
    ASSERT_EQ(admission, rt::LiveControlAdmissionResult::accepted);
    const std::array<rt::ReplayInputRecord, 1> inputs{
        rt::ReplayInputRecord{
            {11, std::chrono::nanoseconds{1'000}}, 1, {}}};
    std::vector<std::byte> input_log(4'096);
    rt::ArtifactWriteResult input_write;
    ASSERT_EQ(
        runtime.write_input_log(inputs, input_log, input_write),
        rt::Status::ok);
    input_log.resize(input_write.bytes_written);

    ASSERT_EQ(
        runtime.step({11, std::chrono::nanoseconds{1'000}}),
        rt::Status::callback_failed);
    rt::LiveControlRecordStatusInfo record_status;
    ASSERT_TRUE(runtime.live_control_record_status(
        kMailbox, 1, record_status));
    EXPECT_EQ(
        record_status.status,
        rt::LiveControlRecordStatus::rolled_back);

    std::vector<std::byte> artifact(64 * 1'024);
    rt::ArtifactWriteResult write;
    ASSERT_EQ(
        runtime.write_live_control_replay_artifact(
            checkpoint,
            input_log,
            rt::LiveControlNestedArtifactKind::input_log,
            artifact,
            write),
        rt::Status::ok);
    artifact.resize(write.bytes_written);
    rt::LiveControlReplayResult replay;
    EXPECT_EQ(
        runtime.replay_live_control(
            artifact, &noop_replay_input, nullptr, &replay),
        rt::Status::callback_failed);
    EXPECT_EQ(replay.mismatch_status, rt::Status::callback_failed);
    EXPECT_EQ(replay.mismatch_action_sequence, 0u);
    EXPECT_EQ(replay.mismatch_target.kind,
              rt::LiveControlTargetKind::host_frame);
    EXPECT_EQ(replay.mismatch_target.frame_index, 11u);
    EXPECT_NE(replay.mismatch_generation_identity, 0u);
    EXPECT_EQ(replay.generations_compared, 1u);
    EXPECT_EQ(replay.frames_replayed, 0u);
    EXPECT_EQ(probe.calls, 2u);
    ASSERT_TRUE(runtime.live_control_record_status(
        kMailbox, 1, record_status));
    EXPECT_EQ(
        record_status.status,
        rt::LiveControlRecordStatus::rolled_back);
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
}
