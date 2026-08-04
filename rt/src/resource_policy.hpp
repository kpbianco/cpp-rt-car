#pragma once

#include <cstddef>

#include <rt/runtime.hpp>

#include "thread_policy.hpp"

namespace rt::detail {

[[nodiscard]] Status build_cpu_memory_policy_report(
    const CpuMemoryPolicy& policy,
    const RuntimeConfig& config,
    const MemoryPlan& memory_plan,
    std::size_t registered_device_buffer_bytes,
    ThreadPolicyProvider& thread_policy_provider,
    CpuMemoryPolicyReport& report,
    const char*& diagnostic) noexcept;

} // namespace rt::detail
