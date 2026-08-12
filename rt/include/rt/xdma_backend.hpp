#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <type_traits>

#include <rt/device.hpp>

namespace rt {

inline constexpr std::uint32_t xdma_driver_api_version_1 = 1;
inline constexpr std::uint32_t xdma_driver_api_version_2 = 2;
inline constexpr std::uint32_t xdma_driver_api_version =
    xdma_driver_api_version_1;
inline constexpr std::uint32_t xdma_driver_api_v1_size = 120;
inline constexpr std::uint32_t xdma_control_aperture_max_bytes = 262144;
inline constexpr std::uint32_t xdma_user_event_capacity = 16;

enum class XdmaDirection : std::uint32_t {
    host_to_card = 1,
    card_to_host = 2,
};

enum class XdmaDriverResult : std::int32_t {
    success = 0,
    invalid_value = 1,
    resource_exhausted = 2,
    timeout = 3,
    io_error = 4,
    device_lost = 5,
    reset_required = 6,
    error = 7,
};

struct XdmaTransferResult {
    XdmaDriverResult result = XdmaDriverResult::error;
    std::uint64_t bytes_transferred = 0;
};

struct XdmaControlReadResult {
    XdmaDriverResult result = XdmaDriverResult::error;
    std::uint32_t value = 0;
};

struct XdmaUserEventResult {
    XdmaDriverResult result = XdmaDriverResult::error;
    std::uint32_t value = 0;
};

/*
 * Blocking transfer belongs exclusively to fixed backend I/O workers.
 * submit() and poll() never call it. The production adapter implements this
 * surface with pread()/pwrite() on the official Xilinx XDMA AXI-MM character
 * devices; tests inject a driver without needing hardware or kernel headers.
 */
struct XdmaDriverApi {
    /* The default and the complete positional prefix remain version 1. */
    std::uint32_t struct_size = xdma_driver_api_v1_size;
    std::uint32_t api_version = xdma_driver_api_version;
    void* user_data = nullptr;

    /*
     * A non-success initialize result may still represent partial driver
     * ownership. The backend makes one immediate shutdown rollback attempt;
     * shutdown must therefore be safe after any initialize call. A shutdown
     * result of success or invalid_value proves that no ownership remains.
     * Any other result must remain retryable by a later shutdown call.
     */
    XdmaDriverResult (*initialize)(void* user_data) noexcept = nullptr;
    XdmaTransferResult (*transfer)(
        void* user_data,
        XdmaDirection direction,
        std::uint32_t channel,
        std::uint64_t device_offset,
        void* host_data,
        std::uint64_t bytes) noexcept = nullptr;
    XdmaDriverResult (*reset)(void* user_data) noexcept = nullptr;
    XdmaDriverResult (*shutdown)(void* user_data) noexcept = nullptr;
    std::uint64_t (*monotonic_time_ns)(void* user_data) noexcept = nullptr;

    std::uint64_t reserved[8]{};

    /* Version-2 tail. Version 1 ignores an entirely zero tail. */
    XdmaControlReadResult (*control_read32)(
        void* user_data,
        std::uint32_t offset) noexcept = nullptr;
    XdmaDriverResult (*control_write32)(
        void* user_data,
        std::uint32_t offset,
        std::uint32_t value) noexcept = nullptr;
    XdmaUserEventResult (*wait_user_event)(
        void* user_data,
        std::uint32_t event_index,
        std::uint64_t timeout_ns) noexcept = nullptr;
    XdmaDriverResult (*request_stop)(void* user_data) noexcept = nullptr;
    std::uint64_t reserved_v2[4]{};
};

static_assert(offsetof(XdmaDriverApi, control_read32) ==
              xdma_driver_api_v1_size);

struct XdmaBackendConfig {
    std::size_t queue_capacity = 64;
    std::size_t buffer_capacity = 64;
    std::size_t worker_count = 1;
    std::uint32_t h2c_channel_count = 1;
    std::uint32_t c2h_channel_count = 1;
    std::uint64_t max_transfer_bytes = 4u * 1024u * 1024u;
    std::uint64_t max_buffer_bytes = 1024u * 1024u * 1024u;
    std::uint64_t transfer_alignment = 1;
    std::uint32_t control_aperture_bytes = 0;
    std::uint32_t user_event_count = 0;
};

inline constexpr std::uint32_t xdma_device_opcode_host_to_card = 0x5844'0001u;
inline constexpr std::uint32_t xdma_device_opcode_card_to_host = 0x5844'0002u;
inline constexpr std::uint32_t xdma_device_opcode_control_read_base =
    0x5848'0000u;
inline constexpr std::uint32_t xdma_device_opcode_control_write_base =
    0x5849'0000u;
inline constexpr std::uint32_t xdma_device_opcode_user_event_base =
    0x584A'0000u;

struct XdmaTransfer {
    std::uint64_t device_offset = 0;
    std::uint32_t channel = 0;
    std::uint32_t reserved0 = 0;
    std::uint64_t reserved[2]{};
};

static_assert(std::is_trivially_copyable_v<XdmaTransfer>);
static_assert(sizeof(XdmaTransfer) <= RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY);

inline void set_xdma_transfer(
    rtfw_device_submission& submission,
    XdmaDirection direction,
    const XdmaTransfer& transfer) noexcept {
    submission.opcode =
        direction == XdmaDirection::host_to_card
        ? xdma_device_opcode_host_to_card
        : xdma_device_opcode_card_to_host;
    submission.payload_size = sizeof(transfer);
    std::memcpy(submission.payload, &transfer, sizeof(transfer));
}

[[nodiscard]] inline bool set_xdma_control_read(
    DeviceCommand& command,
    std::uint32_t offset,
    const HalV2BufferReference& output) noexcept {
    if ((offset & 3u) != 0 || offset >= xdma_control_aperture_max_bytes) {
        return false;
    }
    command = {};
    command.kind = static_cast<std::uint32_t>(HalV2CommandKind::dispatch);
    command.opcode = xdma_device_opcode_control_read_base | (offset / 4u);
    command.buffer_count = 1;
    command.buffers[0] = output;
    return true;
}

[[nodiscard]] inline bool set_xdma_control_write(
    DeviceCommand& command,
    std::uint32_t offset,
    std::uint32_t value) noexcept {
    if ((offset & 3u) != 0 || offset >= xdma_control_aperture_max_bytes) {
        return false;
    }
    command = {};
    command.kind = static_cast<std::uint32_t>(HalV2CommandKind::dispatch);
    command.opcode = xdma_device_opcode_control_write_base | (offset / 4u);
    command.payload_size = sizeof(value);
    command.payload[0] = static_cast<std::uint8_t>(value);
    command.payload[1] = static_cast<std::uint8_t>(value >> 8u);
    command.payload[2] = static_cast<std::uint8_t>(value >> 16u);
    command.payload[3] = static_cast<std::uint8_t>(value >> 24u);
    return true;
}

[[nodiscard]] inline bool set_xdma_user_event_wait(
    DeviceCommand& command,
    std::uint32_t event_index,
    const HalV2BufferReference& output) noexcept {
    if (event_index >= xdma_user_event_capacity) {
        return false;
    }
    command = {};
    command.kind = static_cast<std::uint32_t>(HalV2CommandKind::dispatch);
    command.opcode = xdma_device_opcode_user_event_base | event_index;
    command.buffer_count = 1;
    command.buffers[0] = output;
    return true;
}

class XdmaDeviceBackend final {
public:
    XdmaDeviceBackend(
        const XdmaDriverApi& driver,
        const XdmaBackendConfig& config);
    ~XdmaDeviceBackend();

    XdmaDeviceBackend(XdmaDeviceBackend&&) noexcept;
    XdmaDeviceBackend& operator=(XdmaDeviceBackend&&) noexcept;

    XdmaDeviceBackend(const XdmaDeviceBackend&) = delete;
    XdmaDeviceBackend& operator=(const XdmaDeviceBackend&) = delete;

    [[nodiscard]] rtfw_device_backend_api api() noexcept;
    [[nodiscard]] HalV2BackendRegistration hal_v2_registration(
        std::string_view name) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rt
