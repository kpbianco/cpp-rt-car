#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include <rt/extension_abi.h>
#include <rt/runtime.hpp>

namespace rt::detail {

struct ExtensionRegistrationTransaction {
    std::uint32_t owner = 0;
    std::uint32_t generation = 0;
    std::size_t phase_limit = 0;
    std::size_t backend_limit = 0;
    Status failure = Status::ok;
    rtfw_extension_descriptor_v1 descriptor{};
    std::size_t phase_count = 0;
    std::size_t backend_count = 0;
    std::size_t service_count = 0;
    std::size_t resource_count = 0;
    std::size_t relationship_count = 0;
    std::array<rtfw_extension_phase_v1, RTFW_EXTENSION_PHASE_CAPACITY> phases{};
    std::array<rtfw_extension_backend_v1, RTFW_EXTENSION_BACKEND_CAPACITY>
        backends{};
    std::array<rtfw_extension_service_v1, RTFW_EXTENSION_SERVICE_CAPACITY>
        services{};
    std::array<rtfw_extension_resource_v1, RTFW_EXTENSION_RESOURCE_CAPACITY>
        resources{};
    std::array<
        rtfw_extension_relationship_v1,
        RTFW_EXTENSION_RELATIONSHIP_CAPACITY> relationships{};
};

struct ExtensionPhaseOwner {
    rtfw_frame_callback callback = nullptr;
    void* user_data = nullptr;
    std::atomic<bool> admission{true};
    std::atomic<std::uint32_t> active_calls{0};

    [[nodiscard]] static CallbackResult invoke(
        void* opaque,
        const CallbackContext& context) noexcept;
    void open() noexcept;
    void close() noexcept;
    void clear() noexcept;
};

struct ExtensionBackendOwner {
    rtfw_device_backend_api borrowed{};
    rtfw_device_backend_api forwarding{};
    std::atomic<bool> admission{true};
    std::atomic<bool> released{true};
    std::atomic<std::uint32_t> active_calls{0};

    void initialize_from(const rtfw_device_backend_api& api) noexcept;
    void open() noexcept;
    void close() noexcept;
    void clear() noexcept;

    static rtfw_device_status get_capabilities(
        void*, rtfw_device_capabilities*) noexcept;
    static rtfw_device_status initialize(
        void*, const rtfw_device_init_config*) noexcept;
    static rtfw_device_status register_buffer(
        void*, const rtfw_device_buffer_registration*, uint64_t*) noexcept;
    static rtfw_device_status unregister_buffer(void*, uint64_t) noexcept;
    static rtfw_device_status submit(
        void*, const rtfw_device_submission*) noexcept;
    static rtfw_device_status poll(
        void*, rtfw_device_completion*, uint64_t, uint64_t*) noexcept;
    static rtfw_device_status cancel(void*, uint64_t) noexcept;
    static rtfw_device_status get_health(
        void*, rtfw_device_health*) noexcept;
    static rtfw_device_status reset(void*) noexcept;
    static rtfw_device_status shutdown(void*) noexcept;
};

struct ExtensionServiceOwner {
    rtfw_extension_service_api_v1 api{};
    std::atomic<bool> admission{true};
    std::atomic<bool> initialized{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> quiesced{false};
    std::atomic<bool> released{true};
    std::atomic<std::uint32_t> active_calls{0};

    void initialize_from(const rtfw_extension_service_api_v1& source) noexcept;
    void open() noexcept;
    [[nodiscard]] Status call_initialize() noexcept;
    [[nodiscard]] Status call_request_stop() noexcept;
    [[nodiscard]] Status call_quiesce() noexcept;
    [[nodiscard]] Status call_shutdown() noexcept;
    [[nodiscard]] Status call_status(
        rtfw_extension_service_status_v1& output) noexcept;
    void close() noexcept;
    void clear() noexcept;
};

struct ExtensionRegistrationRecord {
    std::array<char, RTFW_EXTENSION_IDENTIFIER_CAPACITY> name{};
    std::array<char, RTFW_EXTENSION_IDENTIFIER_CAPACITY> version{};
    std::uint32_t negotiated_abi_version = 0;
    std::atomic<std::uint32_t> generation{0};
    std::uint32_t slot = 0;
    std::atomic<ExtensionLifecycleState> state{
        ExtensionLifecycleState::registered};
    std::atomic<bool> unload_ready{false};
    std::size_t phase_count = 0;
    std::size_t backend_count = 0;
    std::size_t service_count = 0;
    std::size_t resource_count = 0;
    std::size_t relationship_count = 0;
    std::array<ExtensionPhaseOwner, RTFW_EXTENSION_PHASE_CAPACITY> phases{};
    std::array<ExtensionBackendOwner, RTFW_EXTENSION_BACKEND_CAPACITY>
        backends{};
    std::array<ExtensionServiceOwner, RTFW_EXTENSION_SERVICE_CAPACITY>
        services{};
    std::array<rtfw_extension_phase_v1, RTFW_EXTENSION_PHASE_CAPACITY>
        phase_descriptors{};
    std::array<rtfw_extension_backend_v1, RTFW_EXTENSION_BACKEND_CAPACITY>
        backend_descriptors{};
    std::array<rtfw_extension_service_v1, RTFW_EXTENSION_SERVICE_CAPACITY>
        service_descriptors{};
    std::array<rtfw_extension_resource_v1, RTFW_EXTENSION_RESOURCE_CAPACITY>
        resource_descriptors{};
    std::array<
        rtfw_extension_relationship_v1,
        RTFW_EXTENSION_RELATIONSHIP_CAPACITY> relationships{};
    std::array<std::uint32_t, RTFW_EXTENSION_PHASE_CAPACITY> phase_indices{};
    std::array<std::uint32_t, RTFW_EXTENSION_BACKEND_CAPACITY> backend_indices{};
    std::array<std::uint32_t, RTFW_EXTENSION_RESOURCE_CAPACITY> resource_indices{};
    std::array<
        std::array<bool, RTFW_EXTENSION_BACKEND_CAPACITY>,
        RTFW_EXTENSION_SERVICE_CAPACITY> service_backend_relationships{};

    void close_admission() noexcept;
    void open_admission() noexcept;
    [[nodiscard]] bool callbacks_quiescent() const noexcept;
    [[nodiscard]] bool backends_released() const noexcept;
    [[nodiscard]] bool services_released() const noexcept;
    void clear_borrowed() noexcept;
};

[[nodiscard]] Status invoke_extension_entry(
    rtfw_extension_entry_fn_v1 entry,
    std::uint32_t owner,
    std::uint32_t generation,
    std::size_t phase_capacity,
    std::size_t backend_capacity,
    ExtensionRegistrationTransaction& transaction) noexcept;

[[nodiscard]] bool valid_extension_identifier(
    const char* value,
    std::size_t capacity) noexcept;
[[nodiscard]] Status status_from_extension(rtfw_status status) noexcept;

} // namespace rt::detail
