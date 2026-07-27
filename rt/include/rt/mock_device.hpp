#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include <rt/device.hpp>

namespace rt {

enum class MockDeviceFault : std::uint8_t {
    none = 0,
    delay = 1,
    timeout = 2,
    error = 3,
    loss = 4,
};

struct MockDeviceFaultRule {
    // One-based accepted-submission ordinal.
    std::uint64_t submission_ordinal = 0;
    MockDeviceFault fault = MockDeviceFault::none;
    std::uint32_t extra_poll_delay = 0;
};

struct MockDeviceConfig {
    std::size_t queue_capacity = 64;
    std::size_t buffer_capacity = 64;
    std::uint32_t default_completion_polls = 1;
    std::uint64_t poll_quantum_ns = 1'000;
};

inline constexpr std::uint32_t mock_device_opcode_noop = 0;
inline constexpr std::uint32_t mock_device_opcode_write_inline = 1;
inline constexpr std::uint32_t mock_device_opcode_fill = 2;

// Deterministic CPU-only backend used to qualify the device contract. It
// creates no thread: Runtime owns the single completion-service lane.
class MockDeviceBackend final {
public:
    explicit MockDeviceBackend(
        const MockDeviceConfig& config = {});
    ~MockDeviceBackend();

    MockDeviceBackend(MockDeviceBackend&&) noexcept;
    MockDeviceBackend& operator=(MockDeviceBackend&&) noexcept;

    MockDeviceBackend(const MockDeviceBackend&) = delete;
    MockDeviceBackend& operator=(const MockDeviceBackend&) = delete;

    [[nodiscard]] rtfw_device_backend_api api() noexcept;
    [[nodiscard]] rtfw_device_status set_fault_script(
        std::span<const MockDeviceFaultRule> rules) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rt
