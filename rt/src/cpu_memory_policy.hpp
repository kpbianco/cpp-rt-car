#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include <rt/runtime.hpp>

namespace rt::detail {

struct ThreadInventoryEntry {
    ThreadResourceId id{};
    ThreadOwnership ownership = ThreadOwnership::runtime;
};

struct MemoryInventoryEntry {
    MemoryRegionId id{};
    MemoryProviderOwnership ownership = MemoryProviderOwnership::runtime;
    MemoryAccountingScope accounting_scope =
        MemoryAccountingScope::runtime_plan;
    std::size_t reported_bytes = 0;
    std::size_t accounted_bytes = 0;
};

[[nodiscard]] Status resolve_thread_policies(
    std::span<const ThreadPolicyRequest> requests,
    std::span<const ThreadInventoryEntry> inventory,
    const ThreadPolicyProviderCapabilities& capabilities,
    std::vector<ThreadPolicyReport>& reports,
    const char*& error) noexcept;

[[nodiscard]] Status resolve_memory_policies(
    std::span<const MemoryPolicyRequest> requests,
    std::span<const MemoryInventoryEntry> inventory,
    std::vector<MemoryRegionPolicyReport>& reports,
    CpuMemoryPolicySummary& summary,
    const char*& error) noexcept;

} // namespace rt::detail
