#include <rt/runtime.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

int main() {
    rt::Runtime runtime;
    rt::LiveControlPolicy policy;
    policy.policy_identity = 0x4d323201;
    policy.mailbox_capacity = 1;
    policy.producer_capacity = 1;
    policy.record_capacity = 1;
    policy.payload_bytes_per_record = 4;
    policy.total_payload_storage_bytes = 4;
    if (runtime.set_live_control_policy(policy) != rt::Status::ok) {
        return 1;
    }

    rt::LiveControlMailboxRegistration mailbox;
    mailbox.mailbox_identity = 1;
    mailbox.record_capacity = 1;
    mailbox.payload_bytes_per_record = 4;
    if (runtime.register_live_control_mailbox(mailbox) != rt::Status::ok) {
        return 2;
    }

    rt::LiveControlProducerRegistration producer;
    producer.mailbox_identity = mailbox.mailbox_identity;
    producer.producer_identity = 2;
    if (runtime.register_live_control_producer(producer) != rt::Status::ok ||
        runtime.finalize() != rt::Status::ok) {
        return 3;
    }

    rt::LiveControlProducerHandle handle;
    if (runtime.live_control_producer_handle(
            mailbox.mailbox_identity,
            producer.producer_identity,
            handle) != rt::Status::ok) {
        return 4;
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
        return 5;
    }

    std::array<std::byte, 4> copied{};
    rt::LiveControlUpdateRecord record;
    if (!runtime.live_control_record_at(1, 1, record) ||
        runtime.copy_live_control_payload(1, 1, copied) != rt::Status::ok ||
        copied != payload || record.payload_digest != update.payload_digest) {
        return 6;
    }
    return runtime.stop() == rt::Status::ok ? 0 : 7;
}
