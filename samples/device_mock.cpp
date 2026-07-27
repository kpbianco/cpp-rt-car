#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <iostream>

#include <rt/mock_device.hpp>
#include <rt/runtime.hpp>

namespace {

struct FillCommand {
    rt::DeviceBufferHandle output{};
    std::size_t bytes = 0;
};

rt::CallbackResult prepare_fill(
    void* user_data,
    const rt::DeviceCallbackContext&,
    rt::DeviceSubmission& submission) {
    const auto& command =
        *static_cast<const FillCommand*>(user_data);
    submission.timeout_ns = 1'000'000;
    submission.opcode = rt::mock_device_opcode_fill;
    submission.payload_size = 1;
    submission.payload[0] = 0x2a;
    submission.buffer_count = 1;
    submission.buffers[0].buffer_token =
        command.output.value;
    submission.buffers[0].access =
        RTFW_DEVICE_ACCESS_WRITE;
    submission.buffers[0].bytes = command.bytes;
    return rt::CallbackResult::ok;
}

rt::CallbackResult validate_output(
    void* user_data,
    const rt::CallbackContext&) {
    const auto& output =
        *static_cast<const std::array<std::byte, 64>*>(
            user_data);
    return std::all_of(
               output.begin(),
               output.end(),
               [](std::byte value) {
                   return value == std::byte{0x2a};
               })
        ? rt::CallbackResult::ok
        : rt::CallbackResult::error;
}

bool check(
    const rt::Runtime& runtime,
    rt::Status status,
    const char* operation) {
    if (status == rt::Status::ok) {
        return true;
    }
    std::cerr << operation << ": " << runtime.last_error() << '\n';
    return false;
}

} // namespace

int main() {
    rt::MockDeviceBackend mock({
        8,
        4,
        2,
        1'000,
    });
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 2;
    config.worker_count = 2;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    config.device_outstanding_capacity = 8;
    config.device_completion_batch = 4;
    if (!check(runtime, runtime.configure(config), "configure")) {
        return 1;
    }

    rt::DeviceBackendHandle backend;
    std::array<std::byte, 64> output{};
    rt::DeviceBufferHandle buffer;
    rt::PhaseHandle fill;
    rt::PhaseHandle validate;
    FillCommand command;
    if (!check(
            runtime,
            runtime.register_device_backend(
                {"sample.mock", mock.api()},
                backend),
            "register backend") ||
        !check(
            runtime,
            runtime.register_device_buffer(
                {"sample.output", backend, output},
                buffer),
            "register buffer")) {
        return 1;
    }
    command = {buffer, output.size()};
    if (!check(
            runtime,
            runtime.register_device_phase(
                {
                    "sample.device-fill",
                    backend,
                    &prepare_fill,
                    &command,
                },
                fill),
            "register device phase") ||
        !check(
            runtime,
            runtime.register_callback(
                {"sample.validate", &validate_output, &output},
                validate),
            "register validation phase") ||
        !check(
            runtime,
            runtime.add_dependency(fill, validate),
            "add dependency") ||
        !check(runtime, runtime.finalize(), "finalize") ||
        !check(runtime, runtime.start(), "start") ||
        !check(
            runtime,
            runtime.step({
                0,
                std::chrono::milliseconds(1),
                std::nullopt,
            }),
            "step") ||
        !check(runtime, runtime.stop(), "stop")) {
        return 1;
    }

    std::cout
        << "device_mock: nonblocking submission completed and released "
           "its dependent CPU phase\n";
    return 0;
}
