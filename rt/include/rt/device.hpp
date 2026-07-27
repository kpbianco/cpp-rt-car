#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include <rt/device_abi.h>

namespace rt {

inline constexpr std::uint64_t invalid_device_handle =
    std::numeric_limits<std::uint64_t>::max();
inline constexpr std::uint32_t device_buffer_kind_bit =
    std::uint32_t{1} << 31u;
inline constexpr std::uint32_t device_handle_index_mask =
    device_buffer_kind_bit - 1u;

struct DeviceBackendHandle {
    std::uint64_t value = invalid_device_handle;

    constexpr DeviceBackendHandle() noexcept = default;
    explicit constexpr DeviceBackendHandle(
        std::uint64_t encoded) noexcept
        : value(encoded) {}
    constexpr DeviceBackendHandle(
        std::uint32_t owner,
        std::uint32_t index) noexcept
        : value(
              (static_cast<std::uint64_t>(owner) << 32u) |
              static_cast<std::uint64_t>(index)) {}

    [[nodiscard]] constexpr bool valid() const noexcept {
        return value != invalid_device_handle &&
               (static_cast<std::uint32_t>(value) &
                device_buffer_kind_bit) == 0;
    }
    [[nodiscard]] constexpr std::uint32_t owner() const noexcept {
        return static_cast<std::uint32_t>(value >> 32u);
    }
    [[nodiscard]] constexpr std::uint32_t index() const noexcept {
        return static_cast<std::uint32_t>(value) &
               device_handle_index_mask;
    }

    friend constexpr bool operator==(
        DeviceBackendHandle,
        DeviceBackendHandle) noexcept = default;
};

struct DeviceBufferHandle {
    std::uint64_t value = invalid_device_handle;

    constexpr DeviceBufferHandle() noexcept = default;
    explicit constexpr DeviceBufferHandle(
        std::uint64_t encoded) noexcept
        : value(encoded) {}
    constexpr DeviceBufferHandle(
        std::uint32_t owner,
        std::uint32_t index) noexcept
        : value(
              (static_cast<std::uint64_t>(owner) << 32u) |
              device_buffer_kind_bit |
              static_cast<std::uint64_t>(index)) {}

    [[nodiscard]] constexpr bool valid() const noexcept {
        return value != invalid_device_handle &&
               (static_cast<std::uint32_t>(value) &
                device_buffer_kind_bit) != 0;
    }
    [[nodiscard]] constexpr std::uint32_t owner() const noexcept {
        return static_cast<std::uint32_t>(value >> 32u);
    }
    [[nodiscard]] constexpr std::uint32_t index() const noexcept {
        return static_cast<std::uint32_t>(value) &
               device_handle_index_mask;
    }

    friend constexpr bool operator==(
        DeviceBufferHandle,
        DeviceBufferHandle) noexcept = default;
};

struct DeviceBackendRegistration {
    std::string_view name;
    rtfw_device_backend_api api{};
};

struct DeviceBufferRegistration {
    std::string_view name;
    DeviceBackendHandle backend{};
    std::span<std::byte> storage{};
    rtfw_device_buffer_flags flags =
        RTFW_DEVICE_BUFFER_HOST_READ |
        RTFW_DEVICE_BUFFER_HOST_WRITE |
        RTFW_DEVICE_BUFFER_DEVICE_READ |
        RTFW_DEVICE_BUFFER_DEVICE_WRITE;
};

using DeviceSubmission = rtfw_device_submission;
using DeviceCapabilities = rtfw_device_capabilities;
using DeviceHealth = rtfw_device_health;

[[nodiscard]] inline DeviceSubmission make_device_submission() noexcept {
    DeviceSubmission submission{};
    submission.struct_size = sizeof(submission);
    submission.abi_version = RTFW_DEVICE_ABI_VERSION;
    return submission;
}

[[nodiscard]] inline DeviceHealth make_device_health() noexcept {
    DeviceHealth health{};
    health.struct_size = sizeof(health);
    return health;
}

} // namespace rt
