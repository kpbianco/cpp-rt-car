#pragma once

#include <cstddef>
#include <cstdint>

#include <rt/device.hpp>
#include <rt/status.hpp>

namespace rt::detail {

struct CommandTimelineExtensionState {
  HalV2CommandTimelineExtension extension{};
  HalV2CommandTimelineCapabilities capabilities{};
};

[[nodiscard]] bool validate_command_timeline_extension(
    const HalV2CommandTimelineExtension &extension) noexcept;
[[nodiscard]] bool validate_command_timeline_capabilities(
    const HalV2CommandTimelineCapabilities &capabilities) noexcept;
[[nodiscard]] Status discover_command_timeline_extension(
    const HalV2CommandTimelineExtension &extension,
    CommandTimelineExtensionState &output) noexcept;

[[nodiscard]] bool
validate_batch_declaration(const DeviceCommandBatch &declaration) noexcept;
[[nodiscard]] bool
validate_batch_shape(const DeviceCommandBatch &batch) noexcept;
[[nodiscard]] bool
batch_matches_declaration(const DeviceCommandBatch &batch,
                          const DeviceCommandBatch &declaration) noexcept;
[[nodiscard]] bool
validate_batch_completion(const HalV2BatchCompletion &completion) noexcept;

} // namespace rt::detail
