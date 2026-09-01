#include <rt/runtime.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* data,
    std::size_t size) {
    constexpr std::size_t maximum_fuzz_bytes = 64 * 1024;
    if (size > maximum_fuzz_bytes) {
        return 0;
    }
    rt::LiveControlReplayMetadata metadata;
    metadata.runtime_id = 0xfeed;
    const auto bytes = std::as_bytes(std::span(data, size));
    const auto status = rt::inspect_live_control_replay_artifact(
        bytes, metadata);
    if (status != rt::Status::ok && metadata.runtime_id != 0) {
        __builtin_trap();
    }
    return 0;
}
