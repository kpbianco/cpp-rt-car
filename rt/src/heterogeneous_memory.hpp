#pragma once

#include <cstddef>
#include <cstdint>

#include <rt/device.hpp>
#include <rt/status.hpp>

namespace rt::detail {

struct HeterogeneousMemoryState {
  bool native_extension = false;
  HalV2MemoryTopologyExtension extension{};
  HalV2MemoryTopologySnapshot snapshot{};
};

[[nodiscard]] bool validate_memory_topology_extension(
    const HalV2MemoryTopologyExtension &extension) noexcept;
[[nodiscard]] bool validate_memory_topology_snapshot(
    const HalV2MemoryTopologySnapshot &snapshot) noexcept;
[[nodiscard]] bool validate_opaque_handle(const HalV2OpaqueHandle &handle,
                                          bool require_nonempty) noexcept;
[[nodiscard]] bool
validate_memory_token(const HalV2MemoryToken &token,
                      bool require_submission_token) noexcept;
[[nodiscard]] bool validate_timestamp_correlation(
    const HalV2TimestampCorrelationQuery &query,
    const HalV2TimestampCorrelation &correlation) noexcept;

[[nodiscard]] Status
discover_memory_topology(const HalV2MemoryTopologyExtension &extension,
                         HeterogeneousMemoryState &output) noexcept;
void make_implicit_host_memory_state(const HalV2Capabilities &capabilities,
                                     HeterogeneousMemoryState &output) noexcept;

[[nodiscard]] const HalV2MemoryDomain *
find_memory_domain(const HeterogeneousMemoryState &state,
                   std::uint64_t identity) noexcept;
[[nodiscard]] const HalV2TimestampDomain *
find_timestamp_domain(const HeterogeneousMemoryState &state,
                      std::uint64_t identity) noexcept;
[[nodiscard]] const HalV2MemoryDomain *
find_legacy_host_domain(const HeterogeneousMemoryState &state,
                        std::uint32_t access) noexcept;

[[nodiscard]] std::uint32_t
ownership_bit(HalV2MemoryOwnership ownership) noexcept;
[[nodiscard]] bool valid_memory_access(std::uint32_t access) noexcept;
[[nodiscard]] bool valid_memory_coherency(std::uint32_t coherency) noexcept;
[[nodiscard]] bool
valid_memory_synchronization(std::uint32_t synchronization) noexcept;

} // namespace rt::detail
