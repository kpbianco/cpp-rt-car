#include <gtest/gtest.h>

#include <rt/loopback_backend.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

namespace {

std::array<std::byte, sizeof(rt::SampledIoFrameHeader) + 8> frame(
    std::uint64_t channel,
    std::uint64_t timestamp_domain,
    std::uint64_t calibration,
    std::uint64_t trigger) {
    std::array<std::byte, sizeof(rt::SampledIoFrameHeader) + 8> bytes{};
    for (std::size_t index = sizeof(rt::SampledIoFrameHeader);
         index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::byte>(index);
    }
    rt::SampledIoFrameHeader header{};
    header.struct_size = sizeof(header);
    header.version = rt::sampled_io_frame_version;
    header.channel_identity = channel;
    header.sequence = 2;
    header.release_generation = 2;
    header.sample_count = 4;
    header.encoding = static_cast<std::uint32_t>(
        rt::SampledIoEncoding::signed_int16_le);
    header.timestamp_domain_identity = timestamp_domain;
    header.sample_interval_ns = 1000;
    header.trigger_identity = trigger;
    header.trigger_sequence = 2;
    header.calibration_identity = calibration;
    header.status = static_cast<std::uint32_t>(
        rt::SampledIoFrameStatus::produced);
    header.payload_checksum = rt::sampled_io_payload_checksum(
        std::span<const std::byte>(bytes).subspan(sizeof(header)));
    std::memcpy(bytes.data(), &header, sizeof(header));
    return bytes;
}

TEST(LoopbackBackend, TransfersCompleteSampledFrameAndRewritesIdentity) {
    rt::SampledIoLoopbackBackend backend;
    rt::SampledIoLoopbackRoute route{};
    route.opcode = 17;
    route.source_channel_identity = 101;
    route.destination_channel_identity = 202;
    route.destination_timestamp_domain_identity = 7;
    route.destination_calibration_identity = 303;
    route.destination_trigger_identity = 404;
    ASSERT_EQ(backend.add_route(route), rt::Status::ok);
    auto registration = backend.hal_v2_registration();
    ASSERT_NE(registration.memory_topology, nullptr);
    ASSERT_NE(registration.command_timeline, nullptr);
    rt::HalV2Capabilities capabilities{};
    ASSERT_EQ(
        registration.api.get_capabilities(
            registration.api.instance, &capabilities),
        rt::HalV2Status::ok);
    ASSERT_EQ(capabilities.max_registered_buffers, 8u);

    rt::HalV2InitializeConfig initialize{};
    initialize.requested_in_flight = 2;
    initialize.requested_registered_buffers = 2;
    ASSERT_EQ(
        registration.api.initialize(registration.api.instance, &initialize),
        rt::HalV2Status::ok);

    auto source = frame(101, 1, 11, 12);
    decltype(source) destination{};
    const auto register_buffer = [&](auto& storage) {
        rt::HalV2BufferRegistration buffer{};
        buffer.data = storage.data();
        buffer.bytes = storage.size();
        buffer.flags = RTFW_DEVICE_BUFFER_HOST_READ |
            RTFW_DEVICE_BUFFER_HOST_WRITE |
            RTFW_DEVICE_BUFFER_DEVICE_READ |
            RTFW_DEVICE_BUFFER_DEVICE_WRITE;
        std::uint64_t token = 0;
        EXPECT_EQ(
            registration.api.register_buffer(
                registration.api.instance, &buffer, &token),
            rt::HalV2Status::ok);
        return token;
    };
    const auto source_token = register_buffer(source);
    const auto destination_token = register_buffer(destination);
    ASSERT_NE(source_token, 0u);
    ASSERT_NE(destination_token, 0u);
    ASSERT_NE(source_token, destination_token);
    ASSERT_LE(source_token, capabilities.max_registered_buffers);
    ASSERT_LE(destination_token, capabilities.max_registered_buffers);

    rt::SampledIoFrameHeader submitted{};
    std::memcpy(&submitted, source.data(), sizeof(submitted));
    ASSERT_EQ(submitted.struct_size, sizeof(submitted));
    ASSERT_EQ(submitted.version, rt::sampled_io_frame_version);
    ASSERT_EQ(submitted.channel_identity, 101u);
    ASSERT_EQ(
        submitted.payload_checksum,
        rt::sampled_io_payload_checksum(
            std::span<const std::byte>(source).subspan(sizeof(submitted))));

    rt::DeviceCommandBatch batch{};
    batch.batch_id = 9;
    batch.timeout_ns = 1000000;
    batch.command_count = 1;
    batch.signal_count = 1;
    rt::DeviceCommand command{};
    command.kind = static_cast<std::uint32_t>(
        rt::HalV2CommandKind::dispatch);
    command.opcode = 17;
    command.buffer_count = 2;
    batch.commands[0] = command;
    batch.signals[0].timeline_handle = 55;
    batch.signals[0].value = 1;
    const auto source_bytes = static_cast<std::uint64_t>(source.size());
    const auto destination_bytes = static_cast<std::uint64_t>(
        destination.size());
    std::memcpy(
        &batch.commands[0].buffers[0].buffer_token,
        &source_token,
        sizeof(source_token));
    std::memcpy(
        &batch.commands[0].buffers[0].bytes,
        &source_bytes,
        sizeof(source_bytes));
    std::memcpy(
        &batch.commands[0].buffers[1].buffer_token,
        &destination_token,
        sizeof(destination_token));
    std::memcpy(
        &batch.commands[0].buffers[1].bytes,
        &destination_bytes,
        sizeof(destination_bytes));
    ASSERT_EQ(batch.commands[0].kind, static_cast<std::uint32_t>(
        rt::HalV2CommandKind::dispatch));
    ASSERT_EQ(batch.commands[0].opcode, route.opcode);
    ASSERT_EQ(batch.commands[0].buffer_count, 2u);
    ASSERT_EQ(batch.commands[0].buffers[0].buffer_token, source_token);
    ASSERT_EQ(batch.commands[0].buffers[1].buffer_token, destination_token);
    ASSERT_EQ(batch.commands[0].buffers[0].bytes, source.size());
    ASSERT_EQ(batch.commands[0].buffers[1].bytes, destination.size());
    ASSERT_EQ(
        registration.command_timeline->submit(
            registration.command_timeline->instance, &batch),
        rt::HalV2Status::ok);

    rt::HalV2BatchCompletion completion{};
    std::uint64_t count = 0;
    ASSERT_EQ(
        registration.command_timeline->poll(
            registration.command_timeline->instance,
            &completion, 1, &count),
        rt::HalV2Status::ok);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(completion.batch_id, 9u);
    EXPECT_EQ(completion.status, static_cast<std::int32_t>(
        rt::HalV2Status::ok));
    EXPECT_EQ(completion.timestamp_domain_identity, 1u);

    rt::SampledIoFrameHeader copied{};
    std::memcpy(&copied, destination.data(), sizeof(copied));
    EXPECT_EQ(copied.channel_identity, 202u);
    EXPECT_EQ(copied.timestamp_domain_identity, 7u);
    EXPECT_EQ(copied.calibration_identity, 303u);
    EXPECT_EQ(copied.trigger_identity, 404u);
    EXPECT_EQ(copied.sequence, 2u);
    EXPECT_TRUE(std::equal(
        source.begin() + static_cast<std::ptrdiff_t>(sizeof(copied)),
        source.end(),
        destination.begin() + static_cast<std::ptrdiff_t>(sizeof(copied))));
    EXPECT_EQ(backend.stats().frames_copied, 1u);
    ASSERT_EQ(backend.inject_next(
        rt::SampledIoLoopbackFault::reject_submission), rt::Status::ok);
    batch.batch_id = 10;
    batch.signals[0].value = 2;
    EXPECT_EQ(registration.command_timeline->submit(
                  registration.command_timeline->instance, &batch),
              rt::HalV2Status::queue_full);
    ASSERT_EQ(backend.inject_next(
        rt::SampledIoLoopbackFault::completion_timeout), rt::Status::ok);
    batch.batch_id = 11;
    ASSERT_EQ(registration.command_timeline->submit(
                  registration.command_timeline->instance, &batch),
              rt::HalV2Status::ok);
    count = 0;
    ASSERT_EQ(registration.command_timeline->poll(
                  registration.command_timeline->instance,
                  &completion, 1, &count),
              rt::HalV2Status::ok);
    EXPECT_EQ(count, 0u);
    ASSERT_EQ(registration.command_timeline->cancel(
                  registration.command_timeline->instance, 11),
              rt::HalV2Status::ok);
    ASSERT_EQ(registration.command_timeline->poll(
                  registration.command_timeline->instance,
                  &completion, 1, &count),
              rt::HalV2Status::ok);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(completion.status, static_cast<std::int32_t>(
        rt::HalV2Status::canceled));

    const auto submit_fault = [&](rt::SampledIoLoopbackFault fault,
                                  std::uint64_t batch_id,
                                  rt::HalV2Status expected_status) {
        ASSERT_EQ(backend.inject_next(fault), rt::Status::ok);
        batch.batch_id = batch_id;
        ++batch.signals[0].value;
        ASSERT_EQ(registration.command_timeline->submit(
                      registration.command_timeline->instance, &batch),
                  rt::HalV2Status::ok);
        count = 0;
        ASSERT_EQ(registration.command_timeline->poll(
                      registration.command_timeline->instance,
                      &completion, 1, &count),
                  rt::HalV2Status::ok);
        ASSERT_EQ(count, 1u);
        EXPECT_EQ(completion.batch_id, batch_id);
        EXPECT_EQ(completion.status, static_cast<std::int32_t>(
            expected_status));
    };
    submit_fault(
        rt::SampledIoLoopbackFault::malformed_sequence,
        12, rt::HalV2Status::ok);
    std::memcpy(&copied, destination.data(), sizeof(copied));
    EXPECT_EQ(copied.sequence, 3u);
    submit_fault(
        rt::SampledIoLoopbackFault::completion_error,
        13, rt::HalV2Status::error);
    submit_fault(
        rt::SampledIoLoopbackFault::completion_lost,
        14, rt::HalV2Status::lost);
    EXPECT_EQ(
        backend.inject_next(rt::SampledIoLoopbackFault::none),
        rt::Status::invalid_argument);
    EXPECT_EQ(
        backend.inject_next(
            static_cast<rt::SampledIoLoopbackFault>(255)),
        rt::Status::invalid_argument);
    EXPECT_EQ(
        registration.api.shutdown(registration.api.instance),
        rt::HalV2Status::ok);
}

TEST(LoopbackBackend, ConfigurationIsBoundedAndInstanceLocal) {
    rt::SampledIoLoopbackBackend first;
    rt::SampledIoLoopbackBackend second;
    EXPECT_EQ(first.add_route({1, 1, 2, 3, 4, 5}), rt::Status::ok);
    EXPECT_EQ(first.add_route({1, 6, 7, 8, 9, 10}),
              rt::Status::invalid_argument);
    EXPECT_EQ(second.add_route({1, 6, 7, 8, 9, 10}), rt::Status::ok);
    EXPECT_EQ(first.stats().submissions, 0u);
    EXPECT_EQ(second.stats().submissions, 0u);
    EXPECT_EQ(first.inject_next(
                  rt::SampledIoLoopbackFault::reject_submission),
              rt::Status::ok);
    EXPECT_EQ(first.inject_next(
                  rt::SampledIoLoopbackFault::completion_error),
              rt::Status::invalid_state);
}

TEST(LoopbackBackend, PairingIdentityIsCanonicalAndFrozenAtRegistration) {
    rt::SampledIoLoopbackBackend first;
    rt::SampledIoLoopbackBackend second;
    ASSERT_EQ(first.add_route({2, 20, 21, 22, 23, 24}), rt::Status::ok);
    ASSERT_EQ(first.add_route({1, 10, 11, 12, 13, 14}), rt::Status::ok);
    ASSERT_EQ(second.add_route({1, 10, 11, 12, 13, 14}), rt::Status::ok);
    ASSERT_EQ(second.add_route({2, 20, 21, 22, 23, 24}), rt::Status::ok);
    const auto a = first.hal_v2_registration("first");
    const auto b = second.hal_v2_registration("second");
    rt::HalV2Capabilities a_capabilities{};
    rt::HalV2Capabilities b_capabilities{};
    ASSERT_EQ(a.api.get_capabilities(a.api.instance, &a_capabilities),
              rt::HalV2Status::ok);
    ASSERT_EQ(b.api.get_capabilities(b.api.instance, &b_capabilities),
              rt::HalV2Status::ok);
    EXPECT_EQ(a_capabilities.backend_id, b_capabilities.backend_id);
    EXPECT_EQ(first.add_route({3, 30, 31, 32, 33, 34}),
              rt::Status::invalid_state);
}

} // namespace
