#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <type_traits>

#include <rt/cuda_backend.hpp>
#include <rt/runtime.hpp>
#include <rt/xdma_backend.hpp>

namespace {

template <typename Range>
bool all_zero(const Range& values) {
    return std::all_of(
        std::begin(values),
        std::end(values),
        [](const auto value) { return value == 0; });
}

rt::CudaDriverApi old_cuda_v1_positional_source() {
    return {
        static_cast<std::uint32_t>(rt::cuda_driver_api_v1_struct_size),
        rt::cuda_driver_api_version_1,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        {},
    };
}

rt::XdmaDriverApi old_xdma_v1_positional_source() {
    return {
        rt::xdma_driver_api_v1_size,
        rt::xdma_driver_api_version_1,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        {},
    };
}

rt::CudaDriverApi complete_cuda_v2_driver() {
    static int driver_identity;
    rt::CudaDriverApi api;
    api.struct_size = rt::cuda_driver_api_v2_struct_size;
    api.api_version = rt::cuda_driver_api_version_2;
    api.user_data = &driver_identity;
    api.push_context = [](void*, rt::CudaContext) noexcept {
        return rt::CudaDriverResult::success;
    };
    api.pop_context = [](void*, rt::CudaContext* output) noexcept {
        if (output) {
            *output = 1;
        }
        return rt::CudaDriverResult::success;
    };
    api.event_create = [](void*, rt::CudaEvent* output) noexcept {
        if (output) {
            *output = 1;
        }
        return rt::CudaDriverResult::success;
    };
    api.event_destroy = [](void*, rt::CudaEvent) noexcept {
        return rt::CudaDriverResult::success;
    };
    api.event_record = [](void*, rt::CudaEvent, rt::CudaStream) noexcept {
        return rt::CudaDriverResult::success;
    };
    api.event_query = [](void*, rt::CudaEvent) noexcept {
        return rt::CudaDriverResult::success;
    };
    api.event_synchronize = [](void*, rt::CudaEvent) noexcept {
        return rt::CudaDriverResult::success;
    };
    api.stream_synchronize = [](void*, rt::CudaStream) noexcept {
        return rt::CudaDriverResult::success;
    };
    api.mem_alloc = [](void*, std::uint64_t, rt::CudaDeviceAddress* output) noexcept {
        if (output) {
            *output = 1;
        }
        return rt::CudaDriverResult::success;
    };
    api.mem_free = [](void*, rt::CudaDeviceAddress) noexcept {
        return rt::CudaDriverResult::success;
    };
    api.host_register = [](void*, void*, std::uint64_t) noexcept {
        return rt::CudaDriverResult::success;
    };
    api.host_unregister = [](void*, void*) noexcept {
        return rt::CudaDriverResult::success;
    };
    api.memcpy_host_to_device_async = [](
        void*, rt::CudaDeviceAddress, const void*, std::uint64_t,
        rt::CudaStream) noexcept { return rt::CudaDriverResult::success; };
    api.memcpy_device_to_host_async = [](
        void*, void*, rt::CudaDeviceAddress, std::uint64_t,
        rt::CudaStream) noexcept { return rt::CudaDriverResult::success; };
    api.memcpy_device_to_device_async = [](
        void*, rt::CudaDeviceAddress, rt::CudaDeviceAddress, std::uint64_t,
        rt::CudaStream) noexcept { return rt::CudaDriverResult::success; };
    api.memset_d8_async = [](
        void*, rt::CudaDeviceAddress, std::uint8_t, std::uint64_t,
        rt::CudaStream) noexcept { return rt::CudaDriverResult::success; };
    api.launch_kernel = [](
        void*, rt::CudaFunction, std::uint32_t, std::uint32_t, std::uint32_t,
        std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
        rt::CudaStream, void* const*) noexcept {
        return rt::CudaDriverResult::success;
    };
    api.monotonic_time_ns = [](void*) noexcept { return std::uint64_t{1}; };
    api.graph_launch = [](void*, rt::CudaGraphExec, rt::CudaStream) noexcept {
        return rt::CudaDriverResult::success;
    };
    return api;
}

rt::XdmaDriverApi complete_xdma_v2_driver() {
    static int driver_identity;
    rt::XdmaDriverApi api;
    api.struct_size = sizeof(api);
    api.api_version = rt::xdma_driver_api_version_2;
    api.user_data = &driver_identity;
    api.initialize = [](void*) noexcept {
        return rt::XdmaDriverResult::success;
    };
    api.transfer = [](
        void*, rt::XdmaDirection, std::uint32_t, std::uint64_t, void*,
        std::uint64_t bytes) noexcept {
        return rt::XdmaTransferResult{rt::XdmaDriverResult::success, bytes};
    };
    api.reset = [](void*) noexcept { return rt::XdmaDriverResult::success; };
    api.shutdown = [](void*) noexcept { return rt::XdmaDriverResult::success; };
    api.monotonic_time_ns = [](void*) noexcept { return std::uint64_t{1}; };
    api.control_read32 = [](void*, std::uint32_t) noexcept {
        return rt::XdmaControlReadResult{rt::XdmaDriverResult::success, 0};
    };
    api.control_write32 = [](void*, std::uint32_t, std::uint32_t) noexcept {
        return rt::XdmaDriverResult::success;
    };
    api.wait_user_event = [](
        void*, std::uint32_t, std::uint64_t) noexcept {
        return rt::XdmaUserEventResult{rt::XdmaDriverResult::success, 0};
    };
    api.request_stop = [](void*) noexcept {
        return rt::XdmaDriverResult::success;
    };
    return api;
}

rt::CallbackResult count_runtime_callback(
    void* user_data,
    const rt::CallbackContext&) {
    static_cast<std::atomic<std::uint64_t>*>(user_data)->fetch_add(
        1, std::memory_order_relaxed);
    return rt::CallbackResult::ok;
}

TEST(VendorHalV2, ExistingDeviceAbiV1SurfaceRemainsExact) {
    EXPECT_EQ(rt::cuda_driver_api_version_1, 1u);
    EXPECT_EQ(rt::cuda_driver_api_version_2, 2u);
    EXPECT_EQ(rt::cuda_driver_api_version, 1u);
    EXPECT_EQ(rt::xdma_driver_api_version_1, 1u);
    EXPECT_EQ(rt::xdma_driver_api_version_2, 2u);
    EXPECT_EQ(rt::xdma_driver_api_version, 1u);
    EXPECT_EQ(rt::cuda_kernel_argument_capacity, 8u);
    EXPECT_EQ(rt::cuda_kernel_scalar_capacity, 48u);
    EXPECT_EQ(rt::cuda_graph_capacity, 16u);
    EXPECT_EQ(rt::cuda_graph_buffer_binding_capacity, 8u);
    EXPECT_EQ(rt::cuda_device_opcode_noop, 0u);
    EXPECT_EQ(rt::cuda_device_opcode_copy_host_to_device, 1u);
    EXPECT_EQ(rt::cuda_device_opcode_copy_device_to_host, 2u);
    EXPECT_EQ(rt::cuda_device_opcode_copy_device_to_device, 3u);
    EXPECT_EQ(rt::cuda_device_opcode_memset_d8, 4u);
    EXPECT_EQ(rt::cuda_device_opcode_launch_kernel, 5u);
    EXPECT_EQ(rt::cuda_device_opcode_graph_base, 0x4347'0000u);
    EXPECT_EQ(rt::cuda_device_opcode_graph(1), 0x4347'0001u);
    EXPECT_EQ(rt::cuda_device_opcode_graph(0xffffu), 0x4347'ffffu);
    EXPECT_EQ(rt::xdma_device_opcode_host_to_card, 0x5844'0001u);
    EXPECT_EQ(rt::xdma_device_opcode_card_to_host, 0x5844'0002u);
    EXPECT_EQ(rt::xdma_device_opcode_control_read_base, 0x5848'0000u);
    EXPECT_EQ(rt::xdma_device_opcode_control_write_base, 0x5849'0000u);
    EXPECT_EQ(rt::xdma_device_opcode_user_event_base, 0x584A'0000u);
    EXPECT_EQ(rt::xdma_control_aperture_max_bytes, 262144u);
    EXPECT_EQ(rt::xdma_user_event_capacity, 16u);

    static_assert(std::is_standard_layout_v<rt::CudaDriverApi>);
    static_assert(std::is_standard_layout_v<rt::XdmaDriverApi>);
    static_assert(std::is_trivially_copyable_v<rt::CudaDriverApi>);
    static_assert(std::is_trivially_copyable_v<rt::XdmaDriverApi>);
    static_assert(sizeof(rt::CudaDeviceAddress) == sizeof(std::uint64_t));
    static_assert(sizeof(rt::CudaGraphExec) == sizeof(std::uintptr_t));
    static_assert(sizeof(rt::CudaGraphBufferBinding) == 24u);
    static_assert(sizeof(rt::XdmaTransfer) == 32u);
    static_assert(sizeof(rt::XdmaControlReadResult) == 8u);
    static_assert(sizeof(rt::XdmaUserEventResult) == 8u);
    EXPECT_EQ(rt::cuda_driver_api_v1_struct_size, 224u);
    EXPECT_EQ(rt::cuda_driver_api_v2_struct_size, 296u);
    EXPECT_EQ(sizeof(rt::XdmaDriverApi), 184u);
    EXPECT_EQ(
        rt::cuda_driver_api_v1_struct_size,
        offsetof(rt::CudaDriverApi, graph_launch));
    EXPECT_EQ(
        rt::cuda_driver_api_v2_struct_size,
        sizeof(rt::CudaDriverApi));

    const rt::CudaDriverApi cuda;
    EXPECT_EQ(cuda.struct_size, rt::cuda_driver_api_v1_struct_size);
    EXPECT_EQ(cuda.api_version, rt::cuda_driver_api_version);
    EXPECT_EQ(cuda.user_data, nullptr);
    EXPECT_EQ(cuda.graph_launch, nullptr);
    EXPECT_TRUE(all_zero(cuda.reserved));
    EXPECT_TRUE(all_zero(cuda.reserved_v2));

    const rt::XdmaDriverApi xdma;
    EXPECT_EQ(xdma.struct_size, rt::xdma_driver_api_v1_size);
    EXPECT_EQ(rt::xdma_driver_api_v1_size, 120u);
    EXPECT_EQ(
        offsetof(rt::XdmaDriverApi, control_read32),
        rt::xdma_driver_api_v1_size);
    EXPECT_EQ(xdma.api_version, rt::xdma_driver_api_version);
    EXPECT_EQ(xdma.user_data, nullptr);
    EXPECT_EQ(xdma.control_read32, nullptr);
    EXPECT_EQ(xdma.control_write32, nullptr);
    EXPECT_EQ(xdma.wait_user_event, nullptr);
    EXPECT_EQ(xdma.request_stop, nullptr);
    EXPECT_TRUE(all_zero(xdma.reserved));
    EXPECT_TRUE(all_zero(xdma.reserved_v2));
}

TEST(VendorHalV2, ExistingConfigurationDefaultsRemainSourceCompatible) {
    const rt::CudaBackendConfig cuda;
    EXPECT_EQ(cuda.queue_capacity, 64u);
    EXPECT_EQ(cuda.buffer_capacity, 64u);
    EXPECT_EQ(cuda.kernel_capacity, 64u);
    EXPECT_EQ(cuda.context, 0u);
    EXPECT_TRUE(cuda.streams.empty());
    EXPECT_TRUE(cuda.allocate_device_mirrors);
    EXPECT_TRUE(cuda.register_host_memory);

    const rt::XdmaBackendConfig xdma;
    EXPECT_EQ(xdma.queue_capacity, 64u);
    EXPECT_EQ(xdma.buffer_capacity, 64u);
    EXPECT_EQ(xdma.worker_count, 1u);
    EXPECT_EQ(xdma.h2c_channel_count, 1u);
    EXPECT_EQ(xdma.c2h_channel_count, 1u);
    EXPECT_EQ(xdma.max_transfer_bytes, 4u * 1024u * 1024u);
    EXPECT_EQ(xdma.max_buffer_bytes, 1024u * 1024u * 1024u);
    EXPECT_EQ(xdma.transfer_alignment, 1u);
    EXPECT_EQ(xdma.control_aperture_bytes, 0u);
    EXPECT_EQ(xdma.user_event_count, 0u);
}

TEST(VendorHalV2, VersionOnePositionalAggregatePrefixesRemainUsable) {
    static_assert(std::is_aggregate_v<rt::CudaDriverApi>);
    static_assert(std::is_aggregate_v<rt::XdmaDriverApi>);

    const auto cuda = old_cuda_v1_positional_source();
    EXPECT_EQ(cuda.struct_size, rt::cuda_driver_api_v1_struct_size);
    EXPECT_EQ(cuda.api_version, rt::cuda_driver_api_version_1);
    EXPECT_EQ(cuda.graph_launch, nullptr);
    EXPECT_TRUE(all_zero(cuda.reserved));
    EXPECT_TRUE(all_zero(cuda.reserved_v2));

    const auto xdma = old_xdma_v1_positional_source();
    EXPECT_EQ(xdma.struct_size, rt::xdma_driver_api_v1_size);
    EXPECT_EQ(xdma.api_version, rt::xdma_driver_api_version_1);
    EXPECT_EQ(xdma.control_read32, nullptr);
    EXPECT_EQ(xdma.control_write32, nullptr);
    EXPECT_EQ(xdma.wait_user_event, nullptr);
    EXPECT_EQ(xdma.request_stop, nullptr);
    EXPECT_TRUE(all_zero(xdma.reserved));
    EXPECT_TRUE(all_zero(xdma.reserved_v2));
}

TEST(VendorHalV2, XdmaControlHelpersEncodeStableLittleEndianCommands) {
    rt::HalV2BufferReference output;
    output.buffer_token = 0x1234u;
    output.access = RTFW_DEVICE_ACCESS_WRITE;
    output.bytes = sizeof(std::uint32_t);

    rt::DeviceCommand read;
    ASSERT_TRUE(rt::set_xdma_control_read(read, 262140u, output));
    EXPECT_EQ(read.kind, static_cast<std::uint32_t>(
                             rt::HalV2CommandKind::dispatch));
    EXPECT_EQ(read.opcode, 0x5848'ffffu);
    EXPECT_EQ(read.payload_size, 0u);
    EXPECT_EQ(read.buffer_count, 1u);
    EXPECT_EQ(read.buffers[0].buffer_token, output.buffer_token);

    rt::DeviceCommand write;
    ASSERT_TRUE(rt::set_xdma_control_write(write, 4u, 0x7856'3412u));
    EXPECT_EQ(write.opcode, 0x5849'0001u);
    EXPECT_EQ(write.payload_size, sizeof(std::uint32_t));
    EXPECT_EQ(write.buffer_count, 0u);
    EXPECT_EQ(write.payload[0], 0x12u);
    EXPECT_EQ(write.payload[1], 0x34u);
    EXPECT_EQ(write.payload[2], 0x56u);
    EXPECT_EQ(write.payload[3], 0x78u);

    rt::DeviceCommand event;
    ASSERT_TRUE(rt::set_xdma_user_event_wait(event, 15u, output));
    EXPECT_EQ(event.opcode, 0x584A'000fu);
    EXPECT_EQ(event.payload_size, 0u);
    EXPECT_EQ(event.buffer_count, 1u);

    EXPECT_FALSE(rt::set_xdma_control_read(read, 1u, output));
    EXPECT_FALSE(rt::set_xdma_control_read(read, 262144u, output));
    EXPECT_FALSE(rt::set_xdma_control_write(write, 3u, 0u));
    EXPECT_FALSE(rt::set_xdma_control_write(write, 262144u, 0u));
    EXPECT_FALSE(rt::set_xdma_user_event_wait(event, 16u, output));
}

TEST(VendorHalV2, CudaNativeRegistrationSuppliesAllFrozenHalTables) {
    auto driver = complete_cuda_v2_driver();
    const std::array<rt::CudaStream, 1> streams{1};
    rt::CudaBackendConfig config;
    config.queue_capacity = 2;
    config.buffer_capacity = 2;
    config.kernel_capacity = 2;
    config.context = 1;
    config.streams = streams;
    rt::CudaDeviceBackend backend(driver, config);

    const auto registration = backend.hal_v2_registration("vendor.cuda");
    EXPECT_EQ(registration.name, "vendor.cuda");
    EXPECT_EQ(registration.api.api_version, rt::hal_v2_api_version);
    EXPECT_NE(registration.api.instance, nullptr);
    ASSERT_NE(registration.memory_topology, nullptr);
    EXPECT_EQ(
        registration.memory_topology->extension_version,
        rt::hal_v2_memory_topology_extension_version);
    EXPECT_NE(registration.memory_topology->instance, nullptr);
    ASSERT_NE(registration.command_timeline, nullptr);
    EXPECT_EQ(
        registration.command_timeline->extension_version,
        rt::hal_v2_command_timeline_extension_version);
    EXPECT_NE(registration.command_timeline->instance, nullptr);

    rt::HalV2Capabilities capabilities;
    ASSERT_EQ(
        registration.api.get_capabilities(
            registration.api.instance, &capabilities),
        rt::HalV2Status::ok);
    EXPECT_EQ(capabilities.max_in_flight, 2u);
    EXPECT_EQ(capabilities.max_registered_buffers, 2u);
    EXPECT_GT(capabilities.control_storage_bytes, 0u);

    rt::HalV2MemoryTopologySnapshot snapshot;
    ASSERT_EQ(
        registration.memory_topology->discover(
            registration.memory_topology->instance, &snapshot),
        rt::HalV2Status::ok);
    EXPECT_EQ(snapshot.memory_domain_count, 2u);
    EXPECT_EQ(snapshot.topology_node_count, 2u);
    EXPECT_EQ(snapshot.timestamp_domain_count, 1u);
    EXPECT_EQ(snapshot.completion_timestamp_domain_identity, 1u);
    EXPECT_EQ(
        snapshot.memory_domains[1].coherency,
        static_cast<std::uint32_t>(
            rt::HalV2MemoryCoherency::staged_copy));
    EXPECT_EQ(
        snapshot.memory_domains[1].required_synchronization,
        rt::hal_v2_memory_sync_copy_to_device |
            rt::hal_v2_memory_sync_copy_from_device);

    rt::HalV2CommandTimelineCapabilities commands;
    ASSERT_EQ(
        registration.command_timeline->get_capabilities(
            registration.command_timeline->instance, &commands),
        rt::HalV2Status::ok);
    EXPECT_EQ(commands.max_in_flight_batches, 2u);
    EXPECT_EQ(commands.max_commands_per_batch, rt::hal_v2_command_capacity);
    EXPECT_GT(commands.backend_control_storage_bytes, 0u);

    const auto legacy = backend.api();
    EXPECT_EQ(legacy.abi_version, RTFW_DEVICE_ABI_VERSION);
    EXPECT_EQ(legacy.instance, registration.api.instance);
}

TEST(VendorHalV2, XdmaNativeRegistrationSuppliesAllFrozenHalTables) {
    auto driver = complete_xdma_v2_driver();
    rt::XdmaBackendConfig config;
    config.queue_capacity = 2;
    config.buffer_capacity = 2;
    config.worker_count = 1;
    config.control_aperture_bytes = 16;
    config.user_event_count = 1;
    rt::XdmaDeviceBackend backend(driver, config);

    const auto registration = backend.hal_v2_registration("vendor.xdma");
    EXPECT_EQ(registration.name, "vendor.xdma");
    EXPECT_EQ(registration.api.api_version, rt::hal_v2_api_version);
    EXPECT_NE(registration.api.instance, nullptr);
    ASSERT_NE(registration.memory_topology, nullptr);
    EXPECT_EQ(
        registration.memory_topology->extension_version,
        rt::hal_v2_memory_topology_extension_version);
    EXPECT_NE(registration.memory_topology->instance, nullptr);
    ASSERT_NE(registration.command_timeline, nullptr);
    EXPECT_EQ(
        registration.command_timeline->extension_version,
        rt::hal_v2_command_timeline_extension_version);
    EXPECT_NE(registration.command_timeline->instance, nullptr);

    rt::HalV2Capabilities capabilities;
    ASSERT_EQ(
        registration.api.get_capabilities(
            registration.api.instance, &capabilities),
        rt::HalV2Status::ok);
    EXPECT_EQ(capabilities.max_in_flight, 2u);
    EXPECT_EQ(capabilities.max_registered_buffers, 2u);
    EXPECT_GT(capabilities.control_storage_bytes, 0u);

    rt::HalV2MemoryTopologySnapshot snapshot;
    ASSERT_EQ(
        registration.memory_topology->discover(
            registration.memory_topology->instance, &snapshot),
        rt::HalV2Status::ok);
    EXPECT_EQ(snapshot.memory_domain_count, 1u);
    EXPECT_EQ(snapshot.topology_node_count, 1u);
    EXPECT_EQ(snapshot.timestamp_domain_count, 1u);
    EXPECT_EQ(snapshot.completion_timestamp_domain_identity, 1u);
    EXPECT_EQ(
        snapshot.memory_domains[0].coherency,
        static_cast<std::uint32_t>(
            rt::HalV2MemoryCoherency::host_coherent));

    rt::HalV2CommandTimelineCapabilities commands;
    ASSERT_EQ(
        registration.command_timeline->get_capabilities(
            registration.command_timeline->instance, &commands),
        rt::HalV2Status::ok);
    EXPECT_EQ(commands.max_in_flight_batches, 2u);
    EXPECT_EQ(commands.max_commands_per_batch, rt::hal_v2_command_capacity);
    EXPECT_GT(commands.backend_control_storage_bytes, 0u);

    const auto legacy = backend.api();
    EXPECT_EQ(legacy.abi_version, RTFW_DEVICE_ABI_VERSION);
    EXPECT_EQ(legacy.instance, registration.api.instance);
}

TEST(VendorHalV2, NativeRuntimeRegistrationDiscoversBothCandidates) {
    auto cuda_driver = complete_cuda_v2_driver();
    const std::array<rt::CudaStream, 1> streams{1};
    rt::CudaBackendConfig cuda_config;
    cuda_config.queue_capacity = 2;
    cuda_config.buffer_capacity = 2;
    cuda_config.kernel_capacity = 2;
    cuda_config.context = 1;
    cuda_config.streams = streams;
    rt::CudaDeviceBackend cuda(cuda_driver, cuda_config);

    auto xdma_driver = complete_xdma_v2_driver();
    rt::XdmaBackendConfig xdma_config;
    xdma_config.queue_capacity = 2;
    xdma_config.buffer_capacity = 2;
    xdma_config.worker_count = 1;
    xdma_config.control_aperture_bytes = 16;
    xdma_config.user_event_count = 1;
    rt::XdmaDeviceBackend xdma(xdma_driver, xdma_config);

    rt::RuntimeConfig config;
    config.callback_capacity = 2;
    config.worker_count = 1;
    config.executor_queue_capacity = 4;
    config.task_scratch_slots = 4;
    config.trace_capacity = 32;
    config.device_backend_capacity = 2;
    config.device_buffer_capacity = 2;
    config.device_outstanding_capacity = 2;
    config.device_completion_batch = 2;
    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(config), rt::Status::ok);

    const auto cuda_registration = cuda.hal_v2_registration("native.cuda");
    const auto xdma_registration = xdma.hal_v2_registration("native.xdma");
    rt::DeviceBackendHandle cuda_handle;
    rt::DeviceBackendHandle xdma_handle;
    ASSERT_EQ(runtime.register_device_backend(cuda_registration, cuda_handle),
              rt::Status::ok);
    ASSERT_EQ(runtime.register_device_backend(xdma_registration, xdma_handle),
              rt::Status::ok);
    EXPECT_TRUE(cuda_handle.valid());
    EXPECT_TRUE(xdma_handle.valid());
    EXPECT_NE(cuda_handle, xdma_handle);
    EXPECT_EQ(runtime.device_backend_count(), 2u);

    std::atomic<std::uint64_t> callback_calls{0};
    rt::PhaseHandle phase;
    ASSERT_EQ(runtime.register_callback(
                  {"native.runtime.cpu", &count_runtime_callback,
                   &callback_calls},
                  phase),
              rt::Status::ok);
    ASSERT_TRUE(phase.valid());
    ASSERT_EQ(runtime.finalize(), rt::Status::ok);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::finalized);
    ASSERT_EQ(runtime.start(), rt::Status::ok);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::running);
    EXPECT_EQ(runtime.step(
                  {7, std::chrono::milliseconds(1), std::nullopt}),
              rt::Status::ok);
    EXPECT_EQ(callback_calls.load(), 1u);
    ASSERT_EQ(runtime.stop(), rt::Status::ok);
    EXPECT_EQ(runtime.state(), rt::RuntimeState::stopped);
}

} // namespace
