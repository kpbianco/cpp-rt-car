#include "command_batch.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace {

template <typename T, std::size_t N>
bool all_zero(const std::array<T, N> &values) noexcept {
  return std::all_of(values.begin(), values.end(),
                     [](const T &value) { return value == T{}; });
}

bool valid_status(std::int32_t value) noexcept {
  return value <= static_cast<std::int32_t>(rt::HalV2Status::ok) &&
         value >= static_cast<std::int32_t>(rt::HalV2Status::reset_required);
}

bool reference_zero(const rt::HalV2BufferReference &reference) noexcept {
  return reference.buffer_token == 0 && reference.access == 0 &&
         reference.reserved0 == 0 && reference.offset == 0 &&
         reference.bytes == 0;
}

bool reference_declared(const rt::HalV2BufferReference &reference) noexcept {
  return reference.buffer_token != 0 && reference.reserved0 == 0 &&
         reference.bytes != 0;
}

bool point_header_valid(const rt::HalV2TimelinePoint &point) noexcept {
  return point.struct_size == sizeof(point) &&
         point.extension_version ==
             rt::hal_v2_command_timeline_extension_version &&
         all_zero(point.reserved);
}

bool inactive_point(const rt::HalV2TimelinePoint &point) noexcept {
  return point_header_valid(point) &&
         point.timeline_handle == rt::invalid_device_handle && point.value == 0;
}

bool command_header_valid(const rt::DeviceCommand &command) noexcept {
  return command.struct_size == sizeof(command) &&
         command.extension_version ==
             rt::hal_v2_command_timeline_extension_version &&
         all_zero(command.reserved);
}

bool inactive_command(const rt::DeviceCommand &command) noexcept {
  if (!command_header_valid(command) ||
      command.kind !=
          static_cast<std::uint32_t>(rt::HalV2CommandKind::invalid) ||
      command.operation !=
          static_cast<std::uint32_t>(rt::HalV2MemoryOperation::invalid) ||
      command.opcode != 0 || command.flags != 0 || command.payload_size != 0 ||
      command.buffer_count != 0 || !all_zero(command.payload) ||
      !reference_zero(command.source) || !reference_zero(command.destination) ||
      !reference_zero(command.target)) {
    return false;
  }
  return std::all_of(command.buffers.begin(), command.buffers.end(),
                     reference_zero);
}

bool active_command(const rt::DeviceCommand &command) noexcept {
  if (!command_header_valid(command)) {
    return false;
  }
  const auto kind = static_cast<rt::HalV2CommandKind>(command.kind);
  const auto operation =
      static_cast<rt::HalV2MemoryOperation>(command.operation);
  if (kind == rt::HalV2CommandKind::dispatch) {
    if (operation != rt::HalV2MemoryOperation::invalid || command.flags != 0 ||
        command.payload_size > rt::hal_v2_inline_payload_capacity ||
        command.buffer_count > rt::hal_v2_buffer_ref_capacity ||
        !reference_zero(command.source) ||
        !reference_zero(command.destination) ||
        !reference_zero(command.target)) {
      return false;
    }
    for (std::size_t index = 0; index < command.buffers.size(); ++index) {
      if (index < command.buffer_count) {
        if (!reference_declared(command.buffers[index])) {
          return false;
        }
      } else if (!reference_zero(command.buffers[index])) {
        return false;
      }
    }
    return std::all_of(command.payload.begin() + command.payload_size,
                       command.payload.end(),
                       [](std::uint8_t value) { return value == 0; });
  }
  if (kind == rt::HalV2CommandKind::copy) {
    if ((operation != rt::HalV2MemoryOperation::copy_to_device &&
         operation != rt::HalV2MemoryOperation::copy_from_device) ||
        command.opcode != 0 || command.flags != 0 ||
        command.payload_size != 0 || command.buffer_count != 0 ||
        !all_zero(command.payload) || !reference_declared(command.source) ||
        !reference_declared(command.destination) ||
        !reference_zero(command.target)) {
      return false;
    }
    return std::all_of(command.buffers.begin(), command.buffers.end(),
                       reference_zero);
  }
  if (kind == rt::HalV2CommandKind::memory_synchronization) {
    if ((operation != rt::HalV2MemoryOperation::flush &&
         operation != rt::HalV2MemoryOperation::invalidate) ||
        command.opcode != 0 || command.flags != 0 ||
        command.payload_size != 0 || command.buffer_count != 0 ||
        !all_zero(command.payload) || !reference_zero(command.source) ||
        !reference_zero(command.destination) ||
        !reference_declared(command.target)) {
      return false;
    }
    return std::all_of(command.buffers.begin(), command.buffers.end(),
                       reference_zero);
  }
  return false;
}

bool same_reference(const rt::HalV2BufferReference &left,
                    const rt::HalV2BufferReference &right) noexcept {
  return left.buffer_token == right.buffer_token &&
         left.access == right.access && left.offset == right.offset &&
         left.bytes == right.bytes;
}

bool command_matches(const rt::DeviceCommand &command,
                     const rt::DeviceCommand &declaration) noexcept {
  if (command.kind != declaration.kind ||
      command.operation != declaration.operation ||
      command.opcode != declaration.opcode ||
      command.flags != declaration.flags ||
      command.buffer_count != declaration.buffer_count) {
    return false;
  }
  for (std::size_t index = 0; index < declaration.buffer_count; ++index) {
    if (!same_reference(command.buffers[index], declaration.buffers[index])) {
      return false;
    }
  }
  return same_reference(command.source, declaration.source) &&
         same_reference(command.destination, declaration.destination) &&
         same_reference(command.target, declaration.target);
}

} // namespace

namespace rt::detail {

bool validate_command_timeline_extension(
    const HalV2CommandTimelineExtension &extension) noexcept {
  return extension.struct_size == sizeof(extension) &&
         extension.extension_version ==
             hal_v2_command_timeline_extension_version &&
         extension.instance != nullptr &&
         extension.get_capabilities != nullptr && extension.submit != nullptr &&
         extension.poll != nullptr && extension.cancel != nullptr &&
         extension.request_stop != nullptr && all_zero(extension.reserved);
}

bool validate_command_timeline_capabilities(
    const HalV2CommandTimelineCapabilities &capabilities) noexcept {
  return capabilities.struct_size == sizeof(capabilities) &&
         capabilities.extension_version ==
             hal_v2_command_timeline_extension_version &&
         capabilities.max_in_flight_batches != 0 &&
         capabilities.max_commands_per_batch != 0 &&
         capabilities.max_commands_per_batch <= hal_v2_command_capacity &&
         capabilities.max_wait_points != 0 &&
         capabilities.max_wait_points <= hal_v2_timeline_wait_capacity &&
         capabilities.max_signal_points != 0 &&
         capabilities.max_signal_points <= hal_v2_timeline_signal_capacity &&
         capabilities.max_timelines != 0 &&
         capabilities.max_timelines <= hal_v2_timeline_capacity &&
         capabilities.completion_batch_capacity != 0 &&
         all_zero(capabilities.reserved);
}

Status discover_command_timeline_extension(
    const HalV2CommandTimelineExtension &extension,
    CommandTimelineExtensionState &output) noexcept {
  if (!validate_command_timeline_extension(extension)) {
    return Status::invalid_argument;
  }
  HalV2CommandTimelineCapabilities capabilities;
  capabilities.struct_size = 0;
  capabilities.extension_version = 0;
  HalV2Status callback_status = HalV2Status::internal_error;
  try {
    callback_status =
        extension.get_capabilities(extension.instance, &capabilities);
  } catch (...) {
    return Status::device_error;
  }
  if (callback_status != HalV2Status::ok) {
    return callback_status == HalV2Status::resource_exhausted
               ? Status::resource_exhausted
               : Status::device_error;
  }
  if (!validate_command_timeline_capabilities(capabilities)) {
    return Status::invalid_argument;
  }
  output.extension = extension;
  output.capabilities = capabilities;
  return Status::ok;
}

bool validate_batch_shape(const DeviceCommandBatch &batch) noexcept {
  if (batch.struct_size != sizeof(batch) ||
      batch.extension_version != hal_v2_command_timeline_extension_version ||
      batch.batch_id != 0 || batch.frame_index != 0 || batch.timeout_ns == 0 ||
      batch.command_count == 0 ||
      batch.command_count > hal_v2_command_capacity ||
      batch.wait_count > hal_v2_timeline_wait_capacity ||
      batch.signal_count == 0 ||
      batch.signal_count > hal_v2_timeline_signal_capacity ||
      batch.reserved0 != 0 || !all_zero(batch.reserved)) {
    return false;
  }
  for (std::size_t index = 0; index < batch.commands.size(); ++index) {
    if (index < batch.command_count
            ? !active_command(batch.commands[index])
            : !inactive_command(batch.commands[index])) {
      return false;
    }
  }
  for (std::size_t index = 0; index < batch.waits.size(); ++index) {
    if (index < batch.wait_count) {
      if (!point_header_valid(batch.waits[index]) ||
          batch.waits[index].timeline_handle == invalid_device_handle ||
          batch.waits[index].value == 0) {
        return false;
      }
    } else if (!inactive_point(batch.waits[index])) {
      return false;
    }
  }
  for (std::size_t index = 0; index < batch.signals.size(); ++index) {
    if (index < batch.signal_count) {
      if (!point_header_valid(batch.signals[index]) ||
          batch.signals[index].timeline_handle == invalid_device_handle ||
          batch.signals[index].value == 0) {
        return false;
      }
    } else if (!inactive_point(batch.signals[index])) {
      return false;
    }
  }
  return true;
}

bool validate_batch_declaration(
    const DeviceCommandBatch &declaration) noexcept {
  for (std::size_t index = 0; index < declaration.wait_count; ++index) {
    if (declaration.waits[index].value != 0) {
      return false;
    }
  }
  for (std::size_t index = 0; index < declaration.signal_count; ++index) {
    if (declaration.signals[index].value != 0) {
      return false;
    }
  }
  auto candidate = declaration;
  candidate.timeout_ns = 1;
  for (std::size_t index = 0; index < candidate.wait_count; ++index) {
    candidate.waits[index].value = 1;
  }
  for (std::size_t index = 0; index < candidate.signal_count; ++index) {
    candidate.signals[index].value = 1;
  }
  return declaration.batch_id == 0 && declaration.frame_index == 0 &&
         declaration.timeout_ns == 0 && validate_batch_shape(candidate);
}

bool batch_matches_declaration(const DeviceCommandBatch &batch,
                               const DeviceCommandBatch &declaration) noexcept {
  if (batch.command_count != declaration.command_count ||
      batch.wait_count != declaration.wait_count ||
      batch.signal_count != declaration.signal_count) {
    return false;
  }
  for (std::size_t index = 0; index < batch.command_count; ++index) {
    if (!command_matches(batch.commands[index], declaration.commands[index])) {
      return false;
    }
  }
  for (std::size_t index = 0; index < batch.wait_count; ++index) {
    if (batch.waits[index].timeline_handle !=
        declaration.waits[index].timeline_handle) {
      return false;
    }
  }
  for (std::size_t index = 0; index < batch.signal_count; ++index) {
    if (batch.signals[index].timeline_handle !=
        declaration.signals[index].timeline_handle) {
      return false;
    }
  }
  return true;
}

bool validate_batch_completion(
    const HalV2BatchCompletion &completion) noexcept {
  if (completion.struct_size != sizeof(completion) ||
      completion.extension_version !=
          hal_v2_command_timeline_extension_version ||
      !valid_status(completion.status) || completion.batch_id == 0 ||
      completion.signal_count == 0 ||
      completion.signal_count > hal_v2_timeline_signal_capacity ||
      completion.timestamp_domain_identity == 0 ||
      !all_zero(completion.reserved)) {
    return false;
  }
  for (std::size_t index = 0; index < completion.signals.size(); ++index) {
    if (index < completion.signal_count) {
      if (!point_header_valid(completion.signals[index]) ||
          completion.signals[index].timeline_handle == invalid_device_handle ||
          completion.signals[index].value == 0) {
        return false;
      }
    } else if (!inactive_point(completion.signals[index])) {
      return false;
    }
  }
  return true;
}

} // namespace rt::detail
