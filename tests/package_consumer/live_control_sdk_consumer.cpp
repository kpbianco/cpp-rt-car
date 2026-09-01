#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include <rt/live_control.hpp>

namespace consumer {

struct Scenario {
    std::uint32_t mode = 0;
    std::uint64_t seed = 0;

    friend bool operator==(Scenario, Scenario) noexcept = default;
};

} // namespace consumer

namespace rt {

template <>
struct LiveControlTypeTraits<consumer::Scenario> {
    static constexpr std::uint32_t application_type_identity = 0x504b'4743u;
    static constexpr std::uint32_t application_schema_version = 1;
    static constexpr LiveControlUpdateKind update_kind =
        LiveControlUpdateKind::scenario_parameters;
    static constexpr std::size_t encoded_extent = 12;

    static bool validate(const consumer::Scenario& value) noexcept {
        return value.mode >= 1 && value.mode <= 4 && value.seed != 0;
    }

    static bool encode(
        const consumer::Scenario& value,
        std::span<std::byte, encoded_extent> output) noexcept {
        return store_u32_le(output, 0, value.mode) &&
            store_u64_le(output, 4, value.seed);
    }

    static bool decode(
        std::span<const std::byte, encoded_extent> input,
        consumer::Scenario& output) noexcept {
        consumer::Scenario candidate;
        if (!load_u32_le(input, 0, candidate.mode) ||
            !load_u64_le(input, 4, candidate.seed)) {
            return false;
        }
        output = candidate;
        return true;
    }
};

} // namespace rt

namespace {

struct Probe {
    consumer::Scenario value{};
    std::size_t records = 0;
};

rt::CallbackResult callback(void* user_data, const rt::CallbackContext& context) {
    auto& probe = *static_cast<Probe*>(user_data);
    if (!context.live_control || context.live_control->records.size() != 1) {
        return rt::CallbackResult::error;
    }
    const auto& view = context.live_control->records.front();
    if (rt::decode_live_control_typed_payload(
            view.record, view.payload, probe.value) !=
        rt::LiveControlTypedStatus::ok) {
        return rt::CallbackResult::error;
    }
    ++probe.records;
    return rt::CallbackResult::ok;
}

} // namespace

static_assert(rt::LiveControlFixedType<consumer::Scenario>);

int main() {
    constexpr std::uint64_t mailbox_identity = 41;
    constexpr std::uint64_t producer_identity = 42;
    constexpr auto payload_bytes = static_cast<std::uint32_t>(
        rt::live_control_typed_payload_extent<consumer::Scenario>);
    rt::Runtime runtime;
    Probe probe;
    if (runtime.register_callback({"typed", callback, &probe}) !=
        rt::Status::ok) {
        return 1;
    }
    rt::LiveControlPolicy policy;
    policy.policy_identity = 0x504b'4704u;
    policy.mailbox_capacity = 1;
    policy.producer_capacity = 1;
    policy.record_capacity = 1;
    policy.payload_bytes_per_record = payload_bytes;
    policy.total_payload_storage_bytes = payload_bytes;
    rt::LiveControlClosurePolicy closure;
    closure.policy_identity = 0x504b'434cu;
    closure.action_capacity = 8;
    rt::LiveControlMailboxRegistration mailbox;
    mailbox.mailbox_identity = mailbox_identity;
    mailbox.record_capacity = 1;
    mailbox.payload_bytes_per_record = payload_bytes;
    rt::LiveControlProducerRegistration producer;
    producer.mailbox_identity = mailbox_identity;
    producer.producer_identity = producer_identity;
    if (runtime.set_live_control_policy(policy) != rt::Status::ok ||
        runtime.set_live_control_closure_policy(closure) != rt::Status::ok ||
        runtime.register_live_control_mailbox(mailbox) != rt::Status::ok ||
        runtime.register_live_control_producer(producer) != rt::Status::ok ||
        runtime.finalize() != rt::Status::ok) {
        return 2;
    }

    rt::LiveControlProducerHandle handle;
    if (runtime.live_control_producer_handle(
            mailbox_identity, producer_identity, handle) != rt::Status::ok) {
        return 3;
    }
    const consumer::Scenario source{2, 0x1122'3344'5566'7788ull};
    rt::LiveControlTypedPayload<consumer::Scenario> payload{};
    rt::LiveControlUpdateRecord host;
    if (rt::make_live_control_host_update(
            handle, 1, 5, source, payload, host) !=
        rt::LiveControlTypedStatus::ok) {
        return 4;
    }
    auto admission = rt::LiveControlAdmissionResult::invalid;
    if (runtime.stage_live_control_update(
            handle, host, payload, admission) != rt::Status::ok ||
        admission != rt::LiveControlAdmissionResult::accepted) {
        return 5;
    }

    rt::LiveControlUpdateRecord full_record;
    if (rt::make_live_control_host_update(
            handle, 2, 5, source, payload, full_record) !=
        rt::LiveControlTypedStatus::ok) {
        return 6;
    }
    admission = rt::LiveControlAdmissionResult::invalid;
    if (runtime.stage_live_control_update(
            handle, full_record, payload, admission) != rt::Status::ok ||
        admission != rt::LiveControlAdmissionResult::full) {
        return 7;
    }

    rt::LiveControlBoundaryTarget rate_target;
    rate_target.kind = rt::LiveControlTargetKind::rate_release;
    rate_target.rate_release_sequence = 9;
    rate_target.reference_release_index = 2;
    rate_target.rate_domain_registration_index = 3;
    rate_target.phase_index = 4;
    rate_target.rate_substep_ordinal = 5;
    rt::LiveControlUpdateRecord rate;
    if (rt::make_live_control_rate_update(
            handle, 2, rate_target, source, payload, rate) !=
            rt::LiveControlTypedStatus::ok ||
        rate.target_kind != rt::LiveControlTargetKind::rate_release ||
        rate.rate_release_sequence != 9 ||
        rate.reference_release_index != 2 ||
        rate.producer_sequence != 2) {
        return 8;
    }

    consumer::Scenario decoded;
    if (rt::decode_live_control_typed_payload(host, payload, decoded) !=
            rt::LiveControlTypedStatus::ok ||
        decoded != source ||
        runtime.start() != rt::Status::ok ||
        runtime.step({5, std::chrono::nanoseconds{1}}) != rt::Status::ok ||
        probe.records != 1 || probe.value != source) {
        return 9;
    }
    std::array<rt::LiveControlActionRecord, 8> actions{};
    rt::LiveControlActionCursor cursor;
    rt::LiveControlActionReadResult read;
    if (runtime.read_live_control_actions(cursor, actions, read) !=
            rt::Status::ok ||
        read.records_read != 4 || read.lost_records != 0 ||
        actions[0].action != rt::LiveControlActionId::admission ||
        actions[1].admission_result != rt::LiveControlAdmissionResult::full ||
        actions[2].action !=
            rt::LiveControlActionId::provisional_publication ||
        actions[3].action != rt::LiveControlActionId::committed) {
        return 10;
    }
    return runtime.stop() == rt::Status::ok ? 0 : 11;
}
