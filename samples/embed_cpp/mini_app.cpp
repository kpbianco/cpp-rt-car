#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>

#include <rt/runtime.hpp>

namespace {

struct SampleState {
    std::uint64_t calls = 0;
    std::uint64_t last_frame = 0;
};

rt::CallbackResult simulate(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& state = *static_cast<SampleState*>(user_data);
    if (context.frame.delta != std::chrono::milliseconds(2) ||
        context.scratch.size() < sizeof(std::uint64_t)) {
        return rt::CallbackResult::error;
    }

    ++state.calls;
    state.last_frame = context.frame.frame_index;
    context.scratch.front() = std::byte{0x2a};
    return rt::CallbackResult::ok;
}

bool check(
    const rt::Runtime& runtime,
    rt::Status status,
    const char* operation) {
    if (status == rt::Status::ok) {
        return true;
    }
    std::cerr << "mini_app_cpp: " << operation << " failed: "
              << runtime.last_error() << '\n';
    return false;
}

} // namespace

int main() {
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    config.callback_capacity = 1;
    config.scratch_bytes = 128;
    config.trace_capacity = 32;

    SampleState state;
    if (!check(runtime, runtime.configure(config), "configure") ||
        !check(
            runtime,
            runtime.register_callback(
                rt::CallbackRegistration{"sample.simulate", &simulate, &state}),
            "register callback") ||
        !check(runtime, runtime.finalize(), "finalize") ||
        !check(runtime, runtime.start(), "start")) {
        return 1;
    }

    for (std::uint64_t frame_index = 0; frame_index < 5; ++frame_index) {
        const auto deadline = runtime.now_ns() + 1'000'000'000u;
        const rt::HostFrameContext frame{
            frame_index,
            std::chrono::milliseconds(2),
            deadline,
        };
        rt::StepResult result;
        if (!check(runtime, runtime.step(frame, &result), "step") ||
            result.callbacks_executed != 1 ||
            result.deadline_missed) {
            return 1;
        }
    }

    if (!check(runtime, runtime.stop(), "stop") ||
        state.calls != 5 ||
        state.last_frame != 4) {
        return 1;
    }

    std::cout << "mini_app_cpp: executed 5 host-driven callbacks\n";
    return 0;
}
