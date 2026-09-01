#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <span>

#include <rt/live_control.hpp>

namespace sample {

struct ScenarioParameters {
    std::uint32_t weather = 0;
    std::uint32_t traffic_density = 0;
    std::uint64_t seed = 0;

    friend bool operator==(
        ScenarioParameters,
        ScenarioParameters) noexcept = default;
};

struct ControllerGains {
    float proportional = 0.0F;
    float integral = 0.0F;
    float derivative = 0.0F;
    std::uint32_t revision = 0;

    friend bool operator==(ControllerGains, ControllerGains) noexcept = default;
};

struct SensorCalibration {
    std::int32_t offset_microvolts = 0;
    std::uint32_t scale_parts_per_million = 0;
    std::uint64_t calibration_identity = 0;

    friend bool operator==(
        SensorCalibration,
        SensorCalibration) noexcept = default;
};

struct FaultConfiguration {
    std::uint32_t fault_identity = 0;
    std::uint32_t mode = 0;
    std::uint64_t activation_count = 0;

    friend bool operator==(
        FaultConfiguration,
        FaultConfiguration) noexcept = default;
};

} // namespace sample

namespace rt {

template <>
struct LiveControlTypeTraits<sample::ScenarioParameters> {
    static constexpr std::uint32_t application_type_identity = 0x5343'4e50u;
    static constexpr std::uint32_t application_schema_version = 1;
    static constexpr LiveControlUpdateKind update_kind =
        LiveControlUpdateKind::scenario_parameters;
    static constexpr std::size_t encoded_extent = 16;

    static bool validate(const sample::ScenarioParameters& value) noexcept {
        return value.weather <= 4 && value.traffic_density <= 100 &&
            value.seed != 0;
    }

    static bool encode(
        const sample::ScenarioParameters& value,
        std::span<std::byte, encoded_extent> output) noexcept {
        return store_u32_le(output, 0, value.weather) &&
            store_u32_le(output, 4, value.traffic_density) &&
            store_u64_le(output, 8, value.seed);
    }

    static bool decode(
        std::span<const std::byte, encoded_extent> input,
        sample::ScenarioParameters& output) noexcept {
        sample::ScenarioParameters candidate;
        if (!load_u32_le(input, 0, candidate.weather) ||
            !load_u32_le(input, 4, candidate.traffic_density) ||
            !load_u64_le(input, 8, candidate.seed)) {
            return false;
        }
        output = candidate;
        return true;
    }
};

template <>
struct LiveControlTypeTraits<sample::ControllerGains> {
    static constexpr std::uint32_t application_type_identity = 0x4354'524cu;
    static constexpr std::uint32_t application_schema_version = 1;
    static constexpr LiveControlUpdateKind update_kind =
        LiveControlUpdateKind::controller_parameters;
    static constexpr std::size_t encoded_extent = 16;

    static bool validate(const sample::ControllerGains& value) noexcept {
        return std::isfinite(value.proportional) &&
            std::isfinite(value.integral) &&
            std::isfinite(value.derivative) &&
            value.proportional >= 0.0F && value.proportional <= 100.0F &&
            value.integral >= 0.0F && value.integral <= 100.0F &&
            value.derivative >= 0.0F && value.derivative <= 100.0F &&
            value.revision != 0;
    }

    static bool encode(
        const sample::ControllerGains& value,
        std::span<std::byte, encoded_extent> output) noexcept {
        return store_u32_le(
                   output, 0, std::bit_cast<std::uint32_t>(value.proportional)) &&
            store_u32_le(
                output, 4, std::bit_cast<std::uint32_t>(value.integral)) &&
            store_u32_le(
                output, 8, std::bit_cast<std::uint32_t>(value.derivative)) &&
            store_u32_le(output, 12, value.revision);
    }

    static bool decode(
        std::span<const std::byte, encoded_extent> input,
        sample::ControllerGains& output) noexcept {
        std::uint32_t proportional = 0;
        std::uint32_t integral = 0;
        std::uint32_t derivative = 0;
        std::uint32_t revision = 0;
        if (!load_u32_le(input, 0, proportional) ||
            !load_u32_le(input, 4, integral) ||
            !load_u32_le(input, 8, derivative) ||
            !load_u32_le(input, 12, revision)) {
            return false;
        }
        output = {
            std::bit_cast<float>(proportional),
            std::bit_cast<float>(integral),
            std::bit_cast<float>(derivative),
            revision};
        return true;
    }
};

template <>
struct LiveControlTypeTraits<sample::SensorCalibration> {
    static constexpr std::uint32_t application_type_identity = 0x5345'4e53u;
    static constexpr std::uint32_t application_schema_version = 1;
    static constexpr LiveControlUpdateKind update_kind =
        LiveControlUpdateKind::sensor_calibration;
    static constexpr std::size_t encoded_extent = 16;

    static bool validate(const sample::SensorCalibration& value) noexcept {
        return value.offset_microvolts >= -1'000'000 &&
            value.offset_microvolts <= 1'000'000 &&
            value.scale_parts_per_million >= 500'000 &&
            value.scale_parts_per_million <= 1'500'000 &&
            value.calibration_identity != 0;
    }

    static bool encode(
        const sample::SensorCalibration& value,
        std::span<std::byte, encoded_extent> output) noexcept {
        return store_u32_le(
                   output,
                   0,
                   std::bit_cast<std::uint32_t>(value.offset_microvolts)) &&
            store_u32_le(output, 4, value.scale_parts_per_million) &&
            store_u64_le(output, 8, value.calibration_identity);
    }

    static bool decode(
        std::span<const std::byte, encoded_extent> input,
        sample::SensorCalibration& output) noexcept {
        std::uint32_t offset = 0;
        sample::SensorCalibration candidate;
        if (!load_u32_le(input, 0, offset) ||
            !load_u32_le(input, 4, candidate.scale_parts_per_million) ||
            !load_u64_le(input, 8, candidate.calibration_identity)) {
            return false;
        }
        candidate.offset_microvolts = std::bit_cast<std::int32_t>(offset);
        output = candidate;
        return true;
    }
};

template <>
struct LiveControlTypeTraits<sample::FaultConfiguration> {
    static constexpr std::uint32_t application_type_identity = 0x4641'554cu;
    static constexpr std::uint32_t application_schema_version = 1;
    static constexpr LiveControlUpdateKind update_kind =
        LiveControlUpdateKind::fault_configuration;
    static constexpr std::size_t encoded_extent = 16;

    static bool validate(const sample::FaultConfiguration& value) noexcept {
        return value.fault_identity != 0 && value.mode >= 1 &&
            value.mode <= 3 && value.activation_count != 0;
    }

    static bool encode(
        const sample::FaultConfiguration& value,
        std::span<std::byte, encoded_extent> output) noexcept {
        return store_u32_le(output, 0, value.fault_identity) &&
            store_u32_le(output, 4, value.mode) &&
            store_u64_le(output, 8, value.activation_count);
    }

    static bool decode(
        std::span<const std::byte, encoded_extent> input,
        sample::FaultConfiguration& output) noexcept {
        sample::FaultConfiguration candidate;
        if (!load_u32_le(input, 0, candidate.fault_identity) ||
            !load_u32_le(input, 4, candidate.mode) ||
            !load_u64_le(input, 8, candidate.activation_count)) {
            return false;
        }
        output = candidate;
        return true;
    }
};

} // namespace rt

namespace {

constexpr std::uint64_t kMailbox = 0x4c49'5645u;
constexpr std::uint64_t kProducer = 0x5341'4d50u;

struct SampleClock final : rt::RuntimeClock {
    std::uint64_t now_ns() noexcept override { return 1'000; }
    rt::Status sleep_until_ns(std::uint64_t) noexcept override {
        return rt::Status::ok;
    }
    bool supports_absolute_sleep() const noexcept override { return true; }
};

struct ObservedState {
    sample::ScenarioParameters scenario{};
    sample::ControllerGains controller{};
    sample::SensorCalibration calibration{};
    sample::FaultConfiguration fault{};
    std::size_t scenario_count = 0;
    std::size_t controller_count = 0;
    std::size_t calibration_count = 0;
    std::size_t fault_count = 0;
    std::size_t clear_count = 0;
    bool reject_next_fault = true;
};

rt::CallbackResult observe(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& state = *static_cast<ObservedState*>(user_data);
    if (!context.live_control) {
        return rt::CallbackResult::ok;
    }
    for (const auto& view : context.live_control->records) {
        switch (view.record.update_kind) {
        case rt::LiveControlUpdateKind::scenario_parameters:
            if (rt::decode_live_control_typed_payload(
                    view.record, view.payload, state.scenario) !=
                rt::LiveControlTypedStatus::ok) {
                return rt::CallbackResult::error;
            }
            ++state.scenario_count;
            break;
        case rt::LiveControlUpdateKind::controller_parameters:
            if (rt::decode_live_control_typed_payload(
                    view.record, view.payload, state.controller) !=
                rt::LiveControlTypedStatus::ok) {
                return rt::CallbackResult::error;
            }
            ++state.controller_count;
            break;
        case rt::LiveControlUpdateKind::sensor_calibration:
            if (rt::decode_live_control_typed_payload(
                    view.record, view.payload, state.calibration) !=
                rt::LiveControlTypedStatus::ok) {
                return rt::CallbackResult::error;
            }
            ++state.calibration_count;
            break;
        case rt::LiveControlUpdateKind::fault_configuration:
            if (rt::decode_live_control_typed_payload(
                    view.record, view.payload, state.fault) !=
                rt::LiveControlTypedStatus::ok) {
                return rt::CallbackResult::error;
            }
            ++state.fault_count;
            if (state.reject_next_fault) {
                state.reject_next_fault = false;
                return rt::CallbackResult::error;
            }
            break;
        case rt::LiveControlUpdateKind::clear_fault:
            if (!view.payload.empty()) {
                return rt::CallbackResult::error;
            }
            ++state.clear_count;
            break;
        }
    }
    return rt::CallbackResult::ok;
}

bool status_ok(
    const rt::Runtime& runtime,
    rt::Status status,
    const char* operation) {
    if (status == rt::Status::ok) {
        return true;
    }
    std::cerr << "live_control_typed: " << operation << " failed: "
              << runtime.last_error() << '\n';
    return false;
}

template <rt::LiveControlFixedType T>
bool stage_host(
    rt::Runtime& runtime,
    rt::LiveControlProducerHandle handle,
    std::uint64_t sequence,
    std::uint64_t frame,
    const T& value) {
    rt::LiveControlTypedPayload<T> payload{};
    rt::LiveControlUpdateRecord update;
    if (rt::make_live_control_host_update(
            handle, sequence, frame, value, payload, update) !=
        rt::LiveControlTypedStatus::ok) {
        return false;
    }
    auto admission = rt::LiveControlAdmissionResult::invalid;
    return runtime.stage_live_control_update(
               handle, update, payload, admission) == rt::Status::ok &&
        admission == rt::LiveControlAdmissionResult::accepted;
}

template <rt::LiveControlFixedType T>
bool stage_rate(
    rt::Runtime& runtime,
    rt::LiveControlProducerHandle handle,
    std::uint64_t sequence,
    const rt::LiveControlBoundaryTarget& target,
    const T& value) {
    rt::LiveControlTypedPayload<T> payload{};
    rt::LiveControlUpdateRecord update;
    if (rt::make_live_control_rate_update(
            handle, sequence, target, value, payload, update) !=
        rt::LiveControlTypedStatus::ok) {
        return false;
    }
    auto admission = rt::LiveControlAdmissionResult::invalid;
    return runtime.stage_live_control_update(
               handle, update, payload, admission) == rt::Status::ok &&
        admission == rt::LiveControlAdmissionResult::accepted;
}

bool run_rate_sample(const sample::SensorCalibration& calibration) {
    SampleClock clock;
    rt::Runtime runtime(clock);
    ObservedState observed;
    observed.reject_next_fault = false;
    rt::PhaseHandle phase;
    if (!status_ok(
            runtime,
            runtime.set_rate_execution_policy({4}),
            "set rate policy") ||
        !status_ok(
            runtime,
            runtime.register_callback(
                {"rate-live-control", &observe, &observed}, phase),
            "register rate callback")) {
        return false;
    }
    rt::RateDomainRegistration registration;
    registration.name = "controller";
    registration.period_ns = 100;
    registration.relative_deadline_ns = 100;
    registration.budget_wcet_ns = 10;
    registration.late_action = rt::RateLateAction::fail;
    rt::RateDomainHandle domain;
    if (!status_ok(
            runtime,
            runtime.register_rate_domain(registration, domain),
            "register rate domain") ||
        !status_ok(
            runtime,
            runtime.bind_phase_to_rate_domain(phase, domain),
            "bind rate callback")) {
        return false;
    }

    constexpr auto payload_capacity = static_cast<std::uint32_t>(
        rt::live_control_typed_payload_extent<sample::SensorCalibration>);
    rt::LiveControlPolicy policy;
    policy.policy_identity = 0x4d32'5204u;
    policy.mailbox_capacity = 1;
    policy.producer_capacity = 1;
    policy.record_capacity = 1;
    policy.payload_bytes_per_record = payload_capacity;
    policy.total_payload_storage_bytes = payload_capacity;
    rt::LiveControlClosurePolicy closure;
    closure.policy_identity = 0x4d32'5243u;
    closure.action_capacity = 8;
    rt::LiveControlMailboxRegistration mailbox;
    mailbox.mailbox_identity = kMailbox;
    mailbox.record_capacity = 1;
    mailbox.payload_bytes_per_record = payload_capacity;
    rt::LiveControlProducerRegistration producer;
    producer.mailbox_identity = kMailbox;
    producer.producer_identity = kProducer;
    if (!status_ok(
            runtime,
            runtime.set_live_control_policy(policy),
            "set rate live-control policy") ||
        !status_ok(
            runtime,
            runtime.set_live_control_closure_policy(closure),
            "set rate closure policy") ||
        !status_ok(
            runtime,
            runtime.register_live_control_mailbox(mailbox),
            "register rate mailbox") ||
        !status_ok(
            runtime,
            runtime.register_live_control_producer(producer),
            "register rate producer") ||
        !status_ok(runtime, runtime.finalize(), "finalize rate runtime")) {
        return false;
    }

    rt::LiveControlProducerHandle handle;
    rt::ReferenceRelease release;
    if (!status_ok(
            runtime,
            runtime.live_control_producer_handle(
                kMailbox, kProducer, handle),
            "get rate producer handle") ||
        !runtime.reference_release_at(0, release)) {
        return false;
    }
    rt::LiveControlBoundaryTarget target;
    target.kind = rt::LiveControlTargetKind::rate_release;
    target.rate_release_sequence = release.domain_release_sequence;
    target.reference_release_index = 0;
    target.rate_domain_registration_index =
        static_cast<std::uint32_t>(release.domain_registration_index);
    target.phase_index = release.phase.index();
    target.rate_substep_ordinal = release.substep_ordinal;
    if (!stage_rate(runtime, handle, 1, target, calibration) ||
        !status_ok(runtime, runtime.start(), "start rate runtime") ||
        !status_ok(
            runtime,
            runtime.step(
                {10, std::chrono::nanoseconds{100}, std::nullopt, 1'000}),
            "exact rate-release step") ||
        observed.calibration != calibration ||
        observed.calibration_count != 1) {
        return false;
    }
    rt::LiveControlCommitInfo commit;
    rt::LiveControlActionMetadata actions;
    return runtime.live_control_commit_info(commit) &&
        commit.committed == 1 && commit.replaced == 0 &&
        runtime.live_control_action_metadata(actions) == rt::Status::ok &&
        actions.records_dropped == 0 &&
        status_ok(runtime, runtime.stop(), "checked rate stop");
}

} // namespace

int main() {
    SampleClock clock;
    rt::Runtime runtime(clock);
    ObservedState observed;
    if (!status_ok(
            runtime,
            runtime.register_callback(
                {"host-live-control", &observe, &observed}),
            "register host callback")) {
        return 1;
    }

    constexpr auto payload_capacity = static_cast<std::uint32_t>(
        rt::live_control_typed_payload_extent<sample::ScenarioParameters>);
    rt::LiveControlPolicy policy;
    policy.policy_identity = 0x4d32'3204u;
    policy.mailbox_capacity = 1;
    policy.producer_capacity = 1;
    policy.record_capacity = 8;
    policy.payload_bytes_per_record = payload_capacity;
    policy.total_payload_storage_bytes =
        static_cast<std::uint64_t>(policy.record_capacity) * payload_capacity;
    rt::LiveControlClosurePolicy closure;
    closure.policy_identity = 0x4d32'4304u;
    closure.action_capacity = 64;
    rt::LiveControlMailboxRegistration mailbox;
    mailbox.mailbox_identity = kMailbox;
    mailbox.record_capacity = policy.record_capacity;
    mailbox.payload_bytes_per_record = payload_capacity;
    rt::LiveControlProducerRegistration producer;
    producer.mailbox_identity = kMailbox;
    producer.producer_identity = kProducer;
    if (!status_ok(
            runtime,
            runtime.set_live_control_policy(policy),
            "set live-control policy") ||
        !status_ok(
            runtime,
            runtime.set_live_control_closure_policy(closure),
            "set closure policy") ||
        !status_ok(
            runtime,
            runtime.register_live_control_mailbox(mailbox),
            "register mailbox") ||
        !status_ok(
            runtime,
            runtime.register_live_control_producer(producer),
            "register producer") ||
        !status_ok(runtime, runtime.finalize(), "finalize")) {
        return 1;
    }

    rt::LiveControlProducerHandle handle;
    if (!status_ok(
            runtime,
            runtime.live_control_producer_handle(
                kMailbox, kProducer, handle),
            "get producer handle")) {
        return 1;
    }

    const sample::ScenarioParameters first_scenario{1, 30, 0x101u};
    const sample::ScenarioParameters final_scenario{2, 45, 0x202u};
    const sample::ControllerGains controller{2.0F, 0.5F, 0.1F, 7};
    if (!stage_host(runtime, handle, 1, 1, first_scenario) ||
        !stage_host(runtime, handle, 2, 1, final_scenario) ||
        !stage_host(runtime, handle, 3, 1, controller) ||
        !status_ok(runtime, runtime.start(), "start") ||
        !status_ok(
            runtime,
            runtime.step({1, std::chrono::nanoseconds{100}}),
            "host step")) {
        return 1;
    }
    if (observed.scenario != final_scenario ||
        observed.controller != controller ||
        observed.scenario_count != 1 || observed.controller_count != 1 ||
        observed.calibration_count != 0) {
        return 1;
    }

    rt::LiveControlCommitInfo commit;
    if (!runtime.live_control_commit_info(commit) || commit.committed != 2 ||
        commit.replaced != 1 || commit.staged_occupancy != 0) {
        return 1;
    }

    const sample::FaultConfiguration fault{17, 2, 1};
    if (!stage_host(runtime, handle, 4, 2, fault) ||
        runtime.step({2, std::chrono::nanoseconds{100}}) !=
            rt::Status::callback_failed ||
        observed.fault != fault || observed.fault_count != 1) {
        return 1;
    }
    rt::LiveControlRecordStatusInfo fault_status;
    if (!runtime.live_control_record_status(kMailbox, 4, fault_status) ||
        fault_status.status != rt::LiveControlRecordStatus::rolled_back) {
        return 1;
    }

    rt::LiveControlUpdateRecord clear;
    clear.runtime_id = handle.runtime_id;
    clear.configuration_generation = handle.configuration_generation;
    clear.mailbox_identity = handle.mailbox_identity;
    clear.producer_identity = handle.producer_identity;
    clear.producer_sequence = 5;
    clear.target_frame_index = 3;
    clear.update_kind = rt::LiveControlUpdateKind::clear_fault;
    clear.payload_bytes = 0;
    clear.payload_digest = rt::live_control_payload_digest({});
    auto admission = rt::LiveControlAdmissionResult::invalid;
    if (runtime.stage_live_control_update(handle, clear, {}, admission) !=
            rt::Status::ok ||
        admission != rt::LiveControlAdmissionResult::accepted ||
        !status_ok(
            runtime,
            runtime.step({3, std::chrono::nanoseconds{100}}),
            "clear-fault correction") ||
        observed.clear_count != 1) {
        return 1;
    }

    std::array<rt::LiveControlActionRecord, 32> actions{};
    rt::LiveControlActionCursor cursor;
    rt::LiveControlActionReadResult read;
    if (runtime.read_live_control_actions(cursor, actions, read) !=
            rt::Status::ok ||
        read.lost_records != 0) {
        return 1;
    }
    bool saw_committed = false;
    bool saw_replaced = false;
    bool saw_rolled_back = false;
    for (std::size_t index = 0; index < read.records_read; ++index) {
        saw_committed = saw_committed ||
            actions[index].action == rt::LiveControlActionId::committed;
        saw_replaced = saw_replaced ||
            actions[index].action == rt::LiveControlActionId::replaced;
        saw_rolled_back = saw_rolled_back ||
            actions[index].action == rt::LiveControlActionId::rolled_back;
    }
    rt::LiveControlActionMetadata action_metadata;
    if (!saw_committed || !saw_replaced || !saw_rolled_back ||
        runtime.live_control_action_metadata(action_metadata) !=
            rt::Status::ok ||
        action_metadata.records_dropped != 0 ||
        !status_ok(runtime, runtime.stop(), "checked stop")) {
        return 1;
    }
    const sample::SensorCalibration calibration{-125, 1'000'250, 0x303u};
    return run_rate_sample(calibration) ? 0 : 1;
}
