#include <rt/runtime.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

static_assert(sizeof(rt::LiveControlClosurePolicy) == 72);
static_assert(sizeof(rt::LiveControlActionRecord) == 256);
static_assert(alignof(rt::LiveControlActionRecord) == 8);

struct Probe {
    std::uint64_t generation = 0;
    std::byte first{};
};

rt::CallbackResult callback(void* data, const rt::CallbackContext& context) {
    auto& probe = *static_cast<Probe*>(data);
    if (!context.live_control || context.live_control->records.size() != 1 ||
        context.live_control->records.front().payload.empty()) {
        return rt::CallbackResult::error;
    }
    probe.generation = context.live_control->generation_identity;
    probe.first = context.live_control->records.front().payload.front();
    return rt::CallbackResult::ok;
}

int main() {
    rt::Runtime runtime;
    Probe probe;
    if (runtime.register_callback({"live-control", callback, &probe}) !=
        rt::Status::ok) {
        return 1;
    }
    rt::LiveControlPolicy policy;
    policy.policy_identity = 0x4d323201;
    policy.mailbox_capacity = 1;
    policy.producer_capacity = 1;
    policy.record_capacity = 1;
    policy.payload_bytes_per_record = 4;
    policy.total_payload_storage_bytes = 4;
    if (runtime.set_live_control_policy(policy) != rt::Status::ok) {
        return 2;
    }
    rt::LiveControlClosurePolicy closure;
    closure.policy_identity = 0x4d323203;
    closure.action_capacity = 8;
    if (runtime.set_live_control_closure_policy(closure) != rt::Status::ok) {
        return 11;
    }

    rt::LiveControlMailboxRegistration mailbox;
    mailbox.mailbox_identity = 1;
    mailbox.record_capacity = 1;
    mailbox.payload_bytes_per_record = 4;
    if (runtime.register_live_control_mailbox(mailbox) != rt::Status::ok) {
        return 3;
    }

    rt::LiveControlProducerRegistration producer;
    producer.mailbox_identity = mailbox.mailbox_identity;
    producer.producer_identity = 2;
    if (runtime.register_live_control_producer(producer) != rt::Status::ok ||
        runtime.finalize() != rt::Status::ok) {
        return 4;
    }

    rt::LiveControlProducerHandle handle;
    if (runtime.live_control_producer_handle(
            mailbox.mailbox_identity,
            producer.producer_identity,
            handle) != rt::Status::ok) {
        return 5;
    }
    const std::array payload{
        std::byte{0x01}, std::byte{0x02},
        std::byte{0x03}, std::byte{0x04}};
    rt::LiveControlUpdateRecord update;
    update.runtime_id = handle.runtime_id;
    update.configuration_generation = handle.configuration_generation;
    update.mailbox_identity = handle.mailbox_identity;
    update.producer_identity = handle.producer_identity;
    update.producer_sequence = 1;
    update.target_frame_index = 10;
    update.payload_bytes = static_cast<std::uint32_t>(payload.size());
    update.payload_digest = rt::live_control_payload_digest(payload);
    rt::LiveControlAdmissionResult admission;
    if (runtime.stage_live_control_update(
            handle, update, payload, admission) != rt::Status::ok ||
        admission != rt::LiveControlAdmissionResult::accepted) {
        return 6;
    }

    std::array<std::byte, 4> copied{};
    rt::LiveControlUpdateRecord record;
    if (!runtime.live_control_record_at(1, 1, record) ||
        runtime.copy_live_control_payload(1, 1, copied) != rt::Status::ok ||
        copied != payload || record.payload_digest != update.payload_digest) {
        return 7;
    }
    if (runtime.start() != rt::Status::ok ||
        runtime.step({10, std::chrono::nanoseconds{1}}) != rt::Status::ok ||
        probe.generation == 0 || probe.first != payload.front()) {
        return 8;
    }
    rt::LiveControlCommitInfo commit;
    if (!runtime.live_control_commit_info(commit) ||
        commit.generation_identity != probe.generation ||
        commit.committed != 1 || commit.staged_occupancy != 0) {
        return 9;
    }
    std::array<rt::LiveControlActionRecord, 4> actions{};
    rt::LiveControlActionCursor cursor;
    rt::LiveControlActionReadResult read;
    if (!runtime.live_control_closure_enabled() ||
        runtime.read_live_control_actions(cursor, actions, read) !=
            rt::Status::ok ||
        read.records_read != 3 || read.lost_records != 0 ||
        actions[0].action != rt::LiveControlActionId::admission ||
        actions[1].action !=
            rt::LiveControlActionId::provisional_publication ||
        actions[2].action != rt::LiveControlActionId::committed) {
        return 12;
    }
    return runtime.stop() == rt::Status::ok ? 0 : 10;
}
