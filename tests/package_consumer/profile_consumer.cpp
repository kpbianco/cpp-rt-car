#include <rt/profile.hpp>

#include <string_view>

namespace {

constexpr std::string_view profile = R"json({
  "schema_version": 1,
  "profile_id": "package.consumer",
  "runtime_compatibility": {"major": 1, "minimum_minor": 1},
  "runtime_config_schema": 7,
  "runtime": {
    "callback_capacity": 8,
    "scratch_bytes": 4096,
    "trace_capacity": 64,
    "numerical_mode": "precise",
    "executor_policy": "static_deterministic",
    "worker_count": 1,
    "executor_queue_capacity": 128,
    "scratch_alignment": 64,
    "task_scratch_bytes": 256,
    "task_scratch_slots": 128,
    "memory_budget_bytes": 268435456,
    "overload_policy": "fail_frame",
    "watchdog_timeout_ns": 0,
    "watchdog_max_degradation_level": 0,
    "platform_preflight_mode": "disabled",
    "determinism_tier": "d0",
    "state_capacity": 8,
    "snapshot_max_bytes": 1048576,
    "replay_input_capacity": 64,
    "input_log_max_bytes": 1048576,
    "device_backend_capacity": 1,
    "device_buffer_capacity": 8,
    "device_outstanding_capacity": 8,
    "device_completion_batch": 8,
    "workload_id": "package.consumer"
  }
})json";

} // namespace

int main() {
    // profile.hpp exposed the Runtime declaration before 1.2; retain that
    // transitive 1.x source-compatibility contract.
    rt::Runtime runtime;
    rt::RuntimeConfig config;
    rt::RuntimeProfileMetadata metadata;
    rt::RuntimeProfileError error;
    if (rt::parse_runtime_profile(
            profile,
            config,
            metadata,
            error) != rt::Status::ok) {
        return 1;
    }
    if (runtime.state() != rt::RuntimeState::configuring ||
        config.worker_count != 1 ||
        config.executor_policy !=
            rt::ExecutorPolicy::static_deterministic ||
        metadata.schema_version != rt::runtime_profile_schema_version ||
        metadata.runtime_config_schema !=
            rt::runtime_config_schema_version ||
        std::string_view(metadata.profile_id.data()) !=
            "package.consumer") {
        return 2;
    }
    return 0;
}
