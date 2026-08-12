#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

#include <rt/device.hpp>

namespace rt {

inline constexpr std::uint32_t cuda_driver_api_version_1 = 1;
inline constexpr std::uint32_t cuda_driver_api_version_2 = 2;
inline constexpr std::uint32_t cuda_driver_api_version =
    cuda_driver_api_version_1;
// Frozen LP64/LLP64 byte size of the complete version-1 positional prefix.
inline constexpr std::size_t cuda_driver_api_v1_struct_size = 224;
inline constexpr std::size_t cuda_kernel_argument_capacity = 8;
inline constexpr std::size_t cuda_kernel_scalar_capacity = 48;
inline constexpr std::size_t cuda_graph_capacity = 16;
inline constexpr std::size_t cuda_graph_buffer_binding_capacity = 8;

using CudaContext = std::uintptr_t;
using CudaStream = std::uintptr_t;
using CudaEvent = std::uintptr_t;
using CudaFunction = std::uintptr_t;
using CudaGraphExec = std::uintptr_t;
using CudaDeviceAddress = std::uint64_t;

enum class CudaDriverResult : std::int32_t {
    success = 0,
    not_ready = 1,
    invalid_value = 2,
    out_of_memory = 3,
    context_lost = 4,
    launch_failure = 5,
    error = 6,
};

/*
 * Narrow, injectable CUDA Driver API surface used by CudaDeviceBackend.
 * Handles intentionally remain opaque so the bounded backend and its tests
 * do not require a CUDA toolkit. The optional rtfw_cuda_driver target supplies
 * the production adapter.
 */
struct CudaDriverApi {
    std::uint32_t struct_size = cuda_driver_api_v1_struct_size;
    std::uint32_t api_version = cuda_driver_api_version;
    void* user_data = nullptr;

    CudaDriverResult (*push_context)(
        void* user_data,
        CudaContext context) noexcept = nullptr;
    CudaDriverResult (*pop_context)(
        void* user_data,
        CudaContext* out_context) noexcept = nullptr;
    CudaDriverResult (*event_create)(
        void* user_data,
        CudaEvent* out_event) noexcept = nullptr;
    CudaDriverResult (*event_destroy)(
        void* user_data,
        CudaEvent event) noexcept = nullptr;
    CudaDriverResult (*event_record)(
        void* user_data,
        CudaEvent event,
        CudaStream stream) noexcept = nullptr;
    CudaDriverResult (*event_query)(
        void* user_data,
        CudaEvent event) noexcept = nullptr;
    CudaDriverResult (*event_synchronize)(
        void* user_data,
        CudaEvent event) noexcept = nullptr;
    CudaDriverResult (*stream_synchronize)(
        void* user_data,
        CudaStream stream) noexcept = nullptr;
    CudaDriverResult (*mem_alloc)(
        void* user_data,
        std::uint64_t bytes,
        CudaDeviceAddress* out_address) noexcept = nullptr;
    CudaDriverResult (*mem_free)(
        void* user_data,
        CudaDeviceAddress address) noexcept = nullptr;
    CudaDriverResult (*host_register)(
        void* user_data,
        void* address,
        std::uint64_t bytes) noexcept = nullptr;
    CudaDriverResult (*host_unregister)(
        void* user_data,
        void* address) noexcept = nullptr;
    CudaDriverResult (*memcpy_host_to_device_async)(
        void* user_data,
        CudaDeviceAddress destination,
        const void* source,
        std::uint64_t bytes,
        CudaStream stream) noexcept = nullptr;
    CudaDriverResult (*memcpy_device_to_host_async)(
        void* user_data,
        void* destination,
        CudaDeviceAddress source,
        std::uint64_t bytes,
        CudaStream stream) noexcept = nullptr;
    CudaDriverResult (*memcpy_device_to_device_async)(
        void* user_data,
        CudaDeviceAddress destination,
        CudaDeviceAddress source,
        std::uint64_t bytes,
        CudaStream stream) noexcept = nullptr;
    CudaDriverResult (*memset_d8_async)(
        void* user_data,
        CudaDeviceAddress destination,
        std::uint8_t value,
        std::uint64_t bytes,
        CudaStream stream) noexcept = nullptr;
    CudaDriverResult (*launch_kernel)(
        void* user_data,
        CudaFunction function,
        std::uint32_t grid_x,
        std::uint32_t grid_y,
        std::uint32_t grid_z,
        std::uint32_t block_x,
        std::uint32_t block_y,
        std::uint32_t block_z,
        std::uint32_t dynamic_shared_bytes,
        CudaStream stream,
        void* const* arguments) noexcept = nullptr;
    std::uint64_t (*monotonic_time_ns)(
        void* user_data) noexcept = nullptr;

    // Frozen version-1 positional prefix ends after this reserved array.
    std::uint64_t reserved[8]{};

    CudaDriverResult (*graph_launch)(
        void* user_data,
        CudaGraphExec graph,
        CudaStream stream) noexcept = nullptr;
    std::uint64_t reserved_v2[8]{};
};

static_assert(offsetof(CudaDriverApi, graph_launch) ==
              cuda_driver_api_v1_struct_size);
inline constexpr std::size_t cuda_driver_api_v2_struct_size =
    sizeof(CudaDriverApi);

struct CudaBackendConfig {
    std::size_t queue_capacity = 64;
    std::size_t buffer_capacity = 64;
    std::size_t kernel_capacity = 64;
    CudaContext context = 0;
    std::span<const CudaStream> streams{};
    bool allocate_device_mirrors = true;
    bool register_host_memory = true;
};

inline constexpr std::uint32_t cuda_device_opcode_noop = 0;
inline constexpr std::uint32_t cuda_device_opcode_copy_host_to_device = 1;
inline constexpr std::uint32_t cuda_device_opcode_copy_device_to_host = 2;
inline constexpr std::uint32_t cuda_device_opcode_copy_device_to_device = 3;
inline constexpr std::uint32_t cuda_device_opcode_memset_d8 = 4;
inline constexpr std::uint32_t cuda_device_opcode_launch_kernel = 5;
inline constexpr std::uint32_t cuda_device_opcode_graph_base = 0x4347'0000u;

[[nodiscard]] constexpr std::uint32_t cuda_device_opcode_graph(
    std::uint16_t graph_id) noexcept {
    return cuda_device_opcode_graph_base |
           static_cast<std::uint32_t>(graph_id);
}

struct CudaGraphBufferBinding {
    std::string_view name{};
    std::uint32_t access = 0;
};

enum class CudaKernelArgumentKind : std::uint8_t {
    none = 0,
    buffer_address = 1,
    scalar = 2,
};

struct CudaKernelArgument {
    std::uint8_t kind = 0;
    std::uint8_t buffer_index = 0;
    std::uint8_t scalar_offset = 0;
    std::uint8_t scalar_size = 0;
};

struct CudaKernelLaunch {
    std::uint64_t kernel_token = 0;
    std::uint32_t grid_x = 1;
    std::uint32_t grid_y = 1;
    std::uint32_t grid_z = 1;
    std::uint32_t block_x = 1;
    std::uint32_t block_y = 1;
    std::uint32_t block_z = 1;
    std::uint32_t dynamic_shared_bytes = 0;
    std::uint8_t argument_count = 0;
    std::uint8_t scalar_data_size = 0;
    std::uint8_t reserved0[2]{};
    std::array<CudaKernelArgument, cuda_kernel_argument_capacity>
        arguments{};
    std::array<std::byte, cuda_kernel_scalar_capacity> scalar_data{};
    std::uint64_t reserved[1]{};
};

static_assert(
    sizeof(CudaKernelLaunch) == RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY);
static_assert(std::is_trivially_copyable_v<CudaKernelLaunch>);

[[nodiscard]] inline bool cuda_kernel_add_buffer_argument(
    CudaKernelLaunch& launch,
    std::uint8_t submission_buffer_index) noexcept {
    if (launch.argument_count >= launch.arguments.size()) {
        return false;
    }
    auto& argument = launch.arguments[launch.argument_count++];
    argument = {};
    argument.kind = static_cast<std::uint8_t>(
        CudaKernelArgumentKind::buffer_address);
    argument.buffer_index = submission_buffer_index;
    return true;
}

[[nodiscard]] inline bool cuda_kernel_add_scalar_argument(
    CudaKernelLaunch& launch,
    std::span<const std::byte> value) noexcept {
    if (launch.argument_count >= launch.arguments.size() ||
        launch.scalar_data_size > launch.scalar_data.size() ||
        value.empty() ||
        value.size() > sizeof(std::uint64_t) ||
        value.size() >
            launch.scalar_data.size() - launch.scalar_data_size) {
        return false;
    }
    auto& argument = launch.arguments[launch.argument_count++];
    argument = {};
    argument.kind =
        static_cast<std::uint8_t>(CudaKernelArgumentKind::scalar);
    argument.scalar_offset = launch.scalar_data_size;
    argument.scalar_size = static_cast<std::uint8_t>(value.size());
    std::memcpy(
        launch.scalar_data.data() + launch.scalar_data_size,
        value.data(),
        value.size());
    launch.scalar_data_size = static_cast<std::uint8_t>(
        launch.scalar_data_size + value.size());
    return true;
}

template <typename T>
[[nodiscard]] inline bool cuda_kernel_add_scalar_argument(
    CudaKernelLaunch& launch,
    const T& value) noexcept
    requires(
        std::is_trivially_copyable_v<T> &&
        sizeof(T) <= sizeof(std::uint64_t)) {
    return cuda_kernel_add_scalar_argument(
        launch,
        std::as_bytes(std::span<const T>(&value, 1)));
}

inline void set_cuda_kernel_launch(
    DeviceSubmission& submission,
    const CudaKernelLaunch& launch) noexcept {
    submission.opcode = cuda_device_opcode_launch_kernel;
    submission.payload_size = sizeof(launch);
    std::memcpy(
        submission.payload,
        &launch,
        sizeof(launch));
}

/*
 * CUDA Driver API backend with fixed queue, buffer, event, stream, and kernel
 * registries. Construction and registration are non-RT setup operations.
 * submit() and poll() allocate no memory, create no threads, and perform no
 * explicit device wait.
 */
class CudaDeviceBackend final {
public:
    CudaDeviceBackend(
        const CudaDriverApi& driver,
        const CudaBackendConfig& config);
    ~CudaDeviceBackend();

    CudaDeviceBackend(CudaDeviceBackend&&) noexcept;
    CudaDeviceBackend& operator=(CudaDeviceBackend&&) noexcept;

    CudaDeviceBackend(const CudaDeviceBackend&) = delete;
    CudaDeviceBackend& operator=(const CudaDeviceBackend&) = delete;

    [[nodiscard]] rtfw_device_backend_api api() noexcept;

    /*
     * Additive native HAL-v2 registration over this same candidate object.
     * The returned tables remain borrowed until checked Runtime shutdown.
     * A candidate object may be initialized through either api() or this
     * registration, never both at once.
     */
    [[nodiscard]] HalV2BackendRegistration hal_v2_registration(
        std::string_view name) noexcept;

    /*
     * Bind a runtime buffer name to caller-owned device storage before
     * initialize(). Otherwise register_buffer() allocates an owned mirror when
     * allocate_device_mirrors is enabled.
     */
    [[nodiscard]] rtfw_device_status bind_device_buffer(
        std::string_view name,
        CudaDeviceAddress address,
        std::uint64_t bytes) noexcept;

    /*
     * Register a caller-owned CUfunction-compatible handle before
     * initialize(). The returned token is encoded in CudaKernelLaunch.
     */
    [[nodiscard]] rtfw_device_status register_kernel(
        CudaFunction function,
        std::uint64_t& out_kernel_token) noexcept;

    /*
     * Register one caller-created and caller-instantiated graph executable.
     * The graph and every named buffer remain caller-owned through checked
     * shutdown. IDs are stable declaration values, never derived from handles.
     */
    [[nodiscard]] rtfw_device_status register_graph(
        std::uint16_t graph_id,
        CudaGraphExec graph,
        std::span<const CudaGraphBufferBinding> bindings) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rt
