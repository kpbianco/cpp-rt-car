#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>

#include <rt/device_abi.h>

namespace rt {

inline constexpr std::uint32_t xdma_driver_api_version = 1;

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

/*
 * Blocking transfer belongs exclusively to fixed backend I/O workers.
 * submit() and poll() never call it. The production adapter implements this
 * surface with pread()/pwrite() on the official Xilinx XDMA AXI-MM character
 * devices; tests inject a driver without needing hardware or kernel headers.
 */
struct XdmaDriverApi {
    std::uint32_t struct_size = sizeof(XdmaDriverApi);
    std::uint32_t api_version = xdma_driver_api_version;
    void* user_data = nullptr;

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
};

struct XdmaBackendConfig {
    std::size_t queue_capacity = 64;
    std::size_t buffer_capacity = 64;
    std::size_t worker_count = 1;
    std::uint32_t h2c_channel_count = 1;
    std::uint32_t c2h_channel_count = 1;
    std::uint64_t max_transfer_bytes = 4u * 1024u * 1024u;
    std::uint64_t max_buffer_bytes = 1024u * 1024u * 1024u;
    std::uint64_t transfer_alignment = 1;
};

inline constexpr std::uint32_t xdma_device_opcode_host_to_card = 0x5844'0001u;
inline constexpr std::uint32_t xdma_device_opcode_card_to_host = 0x5844'0002u;

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rt
