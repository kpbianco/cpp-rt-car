#pragma once

#include <cstdint>

namespace rt {

enum class Status : std::int32_t {
    ok = 0,
    invalid_argument = -1,
    invalid_state = -2,
    invalid_config = -3,
    capacity_exceeded = -4,
    callback_failed = -5,
    resource_exhausted = -6,
    internal_error = -7,
    invalid_handle = -8,
    graph_cycle = -9,
    resource_conflict = -10,
    queue_full = -11,
    scratch_exhausted = -12,
    platform_preflight_failed = -13,
    clock_failure = -14,
    invalid_artifact = -15,
    incompatible_artifact = -16,
    device_queue_full = -17,
    device_timeout = -18,
    device_error = -19,
    device_lost = -20,
    device_canceled = -21,
    device_reset_required = -22,
    incompatible_abi = -23,
};

[[nodiscard]] const char* status_message(Status status) noexcept;

} // namespace rt
