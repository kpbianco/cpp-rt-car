#pragma once

#include "compiled_graph.hpp"
#include "cross_rate_data.hpp"
#include "rate_timeline.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rt::detail {

struct RateAdmissionRecord {
    std::size_t reference_index = 0;
    std::uint64_t declared_start_ns = 0;
    std::uint64_t declared_finish_ns = 0;
    std::uint64_t deadline_ns = 0;
};

struct RateReleaseGroup {
    std::size_t first_reference_index = 0;
    std::size_t reference_count = 0;
    std::size_t domain_registration_index = 0;
    std::uint64_t domain_release_sequence = 0;
    std::uint64_t release_time_ns = 0;
};

struct CompiledRateDispatchPlan {
    RateExecutionPolicy policy{};
    std::vector<RateAdmissionRecord> admission;
    std::vector<RateReleaseGroup> groups;
    // Registration-order domain slices. Each slice contains group indexes in
    // ascending within-supercycle domain release sequence.
    std::array<std::size_t, rate_domain_capacity + 1>
        domain_group_offsets{};
    std::vector<std::size_t> domain_group_indices;
    std::array<std::size_t, rate_domain_capacity>
        records_per_domain_release{};
};

struct RateDispatchDiagnostic {
    Status status = Status::ok;
    const char* message = nullptr;
    std::size_t reference_index = invalid_reference_release_index;
};

struct RateDueCounts {
    std::uint64_t domain_releases = 0;
    std::uint64_t reference_records = 0;
};

// Active-only validation and conservative single-lane declared-budget
// admission. Output is published only after the complete simulation succeeds.
[[nodiscard]] Status compile_rate_dispatch(
    std::uint32_t graph_owner,
    DeterminismTier determinism_tier,
    const RateExecutionPolicy& policy,
    std::span<const GraphDependency> dependencies,
    const CompiledRatePlan& rate_plan,
    std::span<const CrossRateChannelSpec> channels,
    CompiledRateDispatchPlan& output,
    RateDispatchDiagnostic& diagnostic) noexcept;

// Counts the half-open logical interval without iterating through repeated
// supercycles. All arithmetic is checked and the output is transactional.
[[nodiscard]] Status count_due_rate_work(
    const CompiledRatePlan& rate_plan,
    const CompiledRateDispatchPlan& dispatch_plan,
    std::uint64_t cursor_ns,
    std::uint64_t end_ns,
    RateDueCounts& output) noexcept;

[[nodiscard]] Status count_due_domain_releases(
    const CompiledRatePlan& rate_plan,
    const CompiledRateDispatchPlan& dispatch_plan,
    std::size_t domain_index,
    std::uint64_t cursor_ns,
    std::uint64_t end_ns,
    std::uint64_t& domain_releases,
    std::uint64_t& reference_records) noexcept;

} // namespace rt::detail
