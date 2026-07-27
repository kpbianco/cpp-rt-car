#pragma once

#include "executor.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <rt/device.hpp>
#include <rt/runtime.hpp>

namespace rt::detail {

struct DeviceBackendSpec {
    std::string name;
    rtfw_device_backend_api api{};
    rtfw_device_capabilities capabilities{};
};

struct DeviceBufferSpec {
    std::array<char, RTFW_DEVICE_IDENTIFIER_CAPACITY> name{};
    std::uint32_t backend_index = 0;
    std::span<std::byte> storage{};
    rtfw_device_buffer_flags flags = 0;
};

enum class DeviceEventKind : std::uint8_t {
    submitted,
    completed,
    reset,
};

struct DeviceEvent {
    DeviceEventKind kind = DeviceEventKind::submitted;
    Status status = Status::ok;
    std::size_t backend_index = 0;
    std::size_t phase_index = 0;
    std::uint64_t frame_index = 0;
    std::uint64_t submission_id = 0;
    std::uint64_t timestamp_ns = 0;
    RuntimeTraceProducer producer =
        RuntimeTraceProducer::device_service;
    std::size_t worker_index =
        std::numeric_limits<std::size_t>::max();
};

using DeviceEventObserver = void (*)(
    void* user_data,
    const DeviceEvent& event) noexcept;

struct DeviceManagerStats {
    std::uint64_t submissions = 0;
    std::uint64_t completions = 0;
    std::uint64_t failures = 0;
    std::uint64_t queue_rejections = 0;
    std::uint64_t timeouts = 0;
    std::uint64_t losses = 0;
    std::uint64_t resets = 0;
    std::uint64_t service_polls = 0;
    std::uint64_t outstanding = 0;
    std::uint64_t service_starts = 0;
};

class DeviceManager final {
public:
    DeviceManager(
        std::uint32_t owner,
        std::vector<DeviceBackendSpec> backends,
        std::vector<DeviceBufferSpec> buffers,
        std::size_t outstanding_capacity,
        std::size_t completion_batch);
    ~DeviceManager();

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    [[nodiscard]] Status start(
        Executor& executor,
        DeviceEventObserver observer,
        void* observer_data) noexcept;
    void stop() noexcept;

    [[nodiscard]] Status submit(
        std::size_t backend_index,
        std::size_t phase_index,
        std::size_t worker_index,
        std::uint64_t frame_index,
        const DeviceSubmission& submission,
        std::uint64_t& out_submission_id) noexcept;
    [[nodiscard]] Status health(
        std::size_t backend_index,
        DeviceHealth& output) noexcept;
    [[nodiscard]] Status reset(std::size_t backend_index) noexcept;

    [[nodiscard]] DeviceManagerStats stats() const noexcept;
    [[nodiscard]] std::size_t backend_count() const noexcept {
        return backends_.size();
    }
    [[nodiscard]] std::size_t buffer_count() const noexcept {
        return buffers_.size();
    }

    [[nodiscard]] static bool estimate_control_storage(
        std::size_t backend_count,
        std::size_t buffer_count,
        std::size_t outstanding_capacity,
        std::size_t completion_batch,
        std::size_t& bytes) noexcept;

private:
    struct Outstanding {
        std::atomic<std::uint32_t> state{0};
        std::uint64_t submission_id = 0;
        std::uint64_t frame_index = 0;
        std::uint32_t backend_index = 0;
        std::uint32_t phase_index = 0;
        rtfw_device_completion early_completion{};
    };

    [[nodiscard]] Status initialize_backends() noexcept;
    void shutdown_backends() noexcept;
    void service_loop() noexcept;
    void process_completion(
        std::size_t backend_index,
        const rtfw_device_completion& completion) noexcept;
    void finalize_completion(
        Outstanding& slot,
        std::size_t backend_index,
        const rtfw_device_completion& completion) noexcept;
    void fail_backend_outstanding(
        std::size_t backend_index,
        Status status) noexcept;
    void record_failure(Status status) noexcept;
    [[nodiscard]] Outstanding* acquire_outstanding() noexcept;
    void emit(const DeviceEvent& event) noexcept;

    std::uint32_t owner_ = 0;
    std::vector<DeviceBackendSpec> backends_;
    std::vector<std::uint8_t> initialized_backends_;
    std::vector<DeviceBufferSpec> buffers_;
    std::vector<std::uint64_t> native_buffer_tokens_;
    std::unique_ptr<Outstanding[]> outstanding_slots_;
    std::size_t outstanding_capacity_ = 0;
    std::unique_ptr<rtfw_device_completion[]> completion_buffer_;
    std::size_t completion_batch_ = 0;
    std::thread service_thread_;
    Executor* executor_ = nullptr;
    DeviceEventObserver observer_ = nullptr;
    void* observer_data_ = nullptr;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> service_ready_{false};
    std::atomic<std::size_t> slot_hint_{0};
    std::atomic<std::uint64_t> next_submission_id_{1};
    std::atomic<std::uint64_t> submissions_{0};
    std::atomic<std::uint64_t> completions_{0};
    std::atomic<std::uint64_t> failures_{0};
    std::atomic<std::uint64_t> queue_rejections_{0};
    std::atomic<std::uint64_t> timeouts_{0};
    std::atomic<std::uint64_t> losses_{0};
    std::atomic<std::uint64_t> resets_{0};
    std::atomic<std::uint64_t> service_polls_{0};
    std::atomic<std::uint64_t> outstanding_count_{0};
    std::atomic<std::uint64_t> service_starts_{0};
    std::atomic<std::uint64_t> wake_sequence_{0};
};

[[nodiscard]] Status device_status_to_runtime(
    rtfw_device_status status) noexcept;

} // namespace rt::detail
