#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <rt/device.hpp>
#include <rt/status.hpp>

namespace rt::detail {

enum class HalBackendKind : std::uint8_t {
    adapted_device_abi_v1,
    native_hal_v2,
};

[[nodiscard]] bool validate_device_v1_api(
    const rtfw_device_backend_api& api) noexcept;
[[nodiscard]] bool validate_hal_v2_api(
    const HalV2BackendApi& api) noexcept;
[[nodiscard]] bool validate_hal_v2_capabilities(
    const HalV2Capabilities& capabilities) noexcept;
[[nodiscard]] bool validate_hal_v2_health(
    const HalV2Health& health) noexcept;
[[nodiscard]] bool validate_hal_v2_completion(
    const HalV2Completion& completion) noexcept;
[[nodiscard]] Status hal_v2_status_to_runtime(HalV2Status status) noexcept;

class DeviceV1CompatibilityAdapter final {
public:
    explicit DeviceV1CompatibilityAdapter(
        const rtfw_device_backend_api& api) noexcept;

    DeviceV1CompatibilityAdapter(const DeviceV1CompatibilityAdapter&) = delete;
    DeviceV1CompatibilityAdapter& operator=(
        const DeviceV1CompatibilityAdapter&) = delete;

    [[nodiscard]] const HalV2BackendApi& api() const noexcept {
        return api_;
    }
    [[nodiscard]] Status prepare_completion_storage(
        std::size_t capacity) noexcept;
    [[nodiscard]] std::size_t completion_capacity() const noexcept {
        return completion_capacity_;
    }
    [[nodiscard]] const void* completion_storage() const noexcept {
        return completion_storage_.get();
    }

private:
    static HalV2Status get_capabilities(
        void* instance, HalV2Capabilities* capabilities);
    static HalV2Status initialize(
        void* instance, const HalV2InitializeConfig* config);
    static HalV2Status register_buffer(
        void* instance,
        const HalV2BufferRegistration* registration,
        std::uint64_t* out_buffer_token);
    static HalV2Status unregister_buffer(
        void* instance, std::uint64_t buffer_token);
    static HalV2Status submit(
        void* instance, const HalV2Submission* submission);
    static HalV2Status poll(
        void* instance,
        HalV2Completion* completions,
        std::uint64_t completion_capacity,
        std::uint64_t* out_completion_count);
    static HalV2Status cancel(
        void* instance, std::uint64_t submission_id);
    static HalV2Status get_health(
        void* instance, HalV2Health* health);
    static HalV2Status reset(void* instance);
    static HalV2Status shutdown(void* instance);

    rtfw_device_backend_api v1_{};
    HalV2BackendApi api_{};
    std::unique_ptr<rtfw_device_completion[]> completion_storage_;
    std::size_t completion_capacity_ = 0;
};

} // namespace rt::detail
