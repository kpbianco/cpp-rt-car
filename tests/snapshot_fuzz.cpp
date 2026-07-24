#include <rt/runtime.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* data,
    std::size_t size) {
    const auto bytes = std::as_bytes(
        std::span(data, size));
    rt::CheckpointMetadata checkpoint;
    (void)rt::inspect_checkpoint_artifact(
        bytes,
        checkpoint);
    rt::InputLogMetadata input_log;
    (void)rt::inspect_input_log_artifact(
        bytes,
        input_log);
    return 0;
}
