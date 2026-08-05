#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <span>

#include <rt/runtime.hpp>

#include "thread_policy.hpp"

namespace rt::detail {

enum class ControlExtentOwner : std::uint8_t {
    runtime = 0,
    executor = 1,
    device = 2,
};

struct LogicalControlExtent {
    std::uint64_t stable_extent_id = 0;
    ControlExtentOwner owner = ControlExtentOwner::runtime;
    const void* data = nullptr;
    std::size_t bytes = 0;
};

struct ControlExtentExpectation {
    std::size_t extent_count = 0;
    std::size_t accounted_bytes = 0;
};

struct ControlExtentLedger {
    std::array<std::size_t, 3> extent_counts{};
    std::array<std::size_t, 3> accounted_bytes{};
};

[[nodiscard]] Status validate_control_extent_ledger(
    std::span<const LogicalControlExtent> extents,
    const std::array<ControlExtentExpectation, 3>& expected,
    ControlExtentLedger& ledger,
    const char*& diagnostic) noexcept;

[[nodiscard]] Status build_cpu_memory_policy_report(
    const CpuMemoryPolicy& policy,
    const RuntimeConfig& config,
    const MemoryPlan& memory_plan,
    std::size_t registered_device_buffer_bytes,
    const MemoryProvider* memory_provider,
    ThreadPolicyProvider& thread_policy_provider,
    CpuMemoryPolicyReport& report,
    const char*& diagnostic) noexcept;

void refresh_accounting_totals(CpuMemoryPolicyReport& report) noexcept;

[[nodiscard]] Status validate_accounting_closure(
    const CpuMemoryPolicyReport& report,
    bool require_runtime_stack,
    const char*& diagnostic) noexcept;

} // namespace rt::detail
