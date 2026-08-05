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

inline constexpr std::uint32_t hal_v2_api_version = 2u;
inline constexpr std::size_t hal_v2_identifier_capacity =
    RTFW_DEVICE_IDENTIFIER_CAPACITY;
inline constexpr std::size_t hal_v2_inline_payload_capacity =
    RTFW_DEVICE_INLINE_PAYLOAD_CAPACITY;
inline constexpr std::size_t hal_v2_buffer_ref_capacity =
    RTFW_DEVICE_BUFFER_REF_CAPACITY;

enum class HalV2Status : std::int32_t {
    ok = 0,
    invalid_argument = -1,
    invalid_state = -2,
    queue_full = -3,
    timeout = -4,
    error = -5,
    lost = -6,
    canceled = -7,
    unsupported = -8,
    resource_exhausted = -9,
    internal_error = -10,
    reset_required = -11,
};

enum class HalV2HealthState : std::uint32_t {
    shutdown = 0,
    healthy = 1,
    degraded = 2,
    reset_required = 3,
    lost = 4,
};

struct HalV2Capabilities {
    std::uint32_t struct_size = sizeof(HalV2Capabilities);
    std::uint32_t api_version = hal_v2_api_version;
    std::uint64_t max_in_flight = 0;
    std::uint64_t max_registered_buffers = 0;
    std::uint64_t max_buffer_bytes = 0;
    std::uint32_t inline_payload_capacity =
        hal_v2_inline_payload_capacity;
    std::uint32_t buffer_ref_capacity =
        hal_v2_buffer_ref_capacity;
    std::uint8_t supports_cancel = 0;
    std::uint8_t supports_reset = 0;
    std::uint8_t deterministic_mock = 0;
    std::uint8_t reserved0 = 0;
    std::array<char, hal_v2_identifier_capacity> backend_id{};
    std::uint64_t control_storage_bytes = 0;
    std::array<std::uint64_t, 4> reserved{};
};

struct HalV2InitializeConfig {
    std::uint32_t struct_size = sizeof(HalV2InitializeConfig);
    std::uint32_t api_version = hal_v2_api_version;
    std::uint64_t requested_in_flight = 0;
    std::uint64_t requested_registered_buffers = 0;
    std::array<std::uint64_t, 4> reserved{};
};

struct HalV2BufferRegistration {
    std::uint32_t struct_size = sizeof(HalV2BufferRegistration);
    std::uint32_t flags = 0;
    void* data = nullptr;
    std::uint64_t bytes = 0;
    std::array<char, hal_v2_identifier_capacity> name{};
    std::array<std::uint64_t, 4> reserved{};
};

struct HalV2BufferReference {
    std::uint64_t buffer_token = 0;
    std::uint32_t access = 0;
    std::uint32_t reserved0 = 0;
    std::uint64_t offset = 0;
    std::uint64_t bytes = 0;
};

struct HalV2Submission {
    std::uint32_t struct_size = sizeof(HalV2Submission);
    std::uint32_t api_version = hal_v2_api_version;
    std::uint64_t submission_id = 0;
    std::uint64_t frame_index = 0;
    std::uint64_t timeout_ns = 0;
    std::uint32_t opcode = 0;
    std::uint32_t flags = 0;
    std::uint32_t payload_size = 0;
    std::uint32_t buffer_count = 0;
    std::array<std::uint8_t, hal_v2_inline_payload_capacity> payload{};
    std::array<HalV2BufferReference, hal_v2_buffer_ref_capacity> buffers{};
    std::array<std::uint64_t, 4> reserved{};
};

struct HalV2Completion {
    std::uint32_t struct_size = sizeof(HalV2Completion);
    std::int32_t status = static_cast<std::int32_t>(HalV2Status::ok);
    std::uint64_t submission_id = 0;
    std::uint64_t device_timestamp_ns = 0;
    std::uint64_t value = 0;
    std::array<std::uint64_t, 4> reserved{};
};

struct HalV2Health {
    std::uint32_t struct_size = sizeof(HalV2Health);
    std::uint32_t state =
        static_cast<std::uint32_t>(HalV2HealthState::shutdown);
    std::int32_t last_status = static_cast<std::int32_t>(HalV2Status::ok);
    std::uint32_t reserved0 = 0;
    std::uint64_t generation = 0;
    std::uint64_t submissions = 0;
    std::uint64_t completions = 0;
    std::uint64_t queue_rejections = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t errors = 0;
    std::uint64_t losses = 0;
    std::uint64_t cancellations = 0;
    std::uint64_t resets = 0;
    std::uint64_t outstanding = 0;
    std::array<std::uint64_t, 4> reserved{};
};

using HalV2GetCapabilitiesFn = HalV2Status (*)(
    void*, HalV2Capabilities*);
using HalV2InitializeFn = HalV2Status (*)(
    void*, const HalV2InitializeConfig*);
using HalV2RegisterBufferFn = HalV2Status (*)(
    void*, const HalV2BufferRegistration*, std::uint64_t*);
using HalV2UnregisterBufferFn = HalV2Status (*)(void*, std::uint64_t);
using HalV2SubmitFn = HalV2Status (*)(void*, const HalV2Submission*);
using HalV2PollFn = HalV2Status (*)(
    void*, HalV2Completion*, std::uint64_t, std::uint64_t*);
using HalV2CancelFn = HalV2Status (*)(void*, std::uint64_t);
using HalV2GetHealthFn = HalV2Status (*)(void*, HalV2Health*);
using HalV2ResetFn = HalV2Status (*)(void*);
using HalV2ShutdownFn = HalV2Status (*)(void*);

/*
 * The table is copied by Runtime. Its non-null instance remains borrowed
 * until checked stop succeeds. Operations must not throw. Submission and poll
 * are bounded and nonblocking, and poll must not invoke host callbacks.
 */
struct HalV2BackendApi {
    std::uint32_t struct_size = sizeof(HalV2BackendApi);
    std::uint32_t api_version = hal_v2_api_version;
    void* instance = nullptr;
    HalV2GetCapabilitiesFn get_capabilities = nullptr;
    HalV2InitializeFn initialize = nullptr;
    HalV2RegisterBufferFn register_buffer = nullptr;
    HalV2UnregisterBufferFn unregister_buffer = nullptr;
    HalV2SubmitFn submit = nullptr;
    HalV2PollFn poll = nullptr;
    HalV2CancelFn cancel = nullptr;
    HalV2GetHealthFn get_health = nullptr;
    HalV2ResetFn reset = nullptr;
    HalV2ShutdownFn shutdown = nullptr;
    std::array<std::uint64_t, 8> reserved{};
};

struct HalV2BackendRegistration {
    std::string_view name;
    HalV2BackendApi api{};
};

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
