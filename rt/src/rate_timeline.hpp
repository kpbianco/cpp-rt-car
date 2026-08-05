#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <rt/runtime.hpp>

namespace rt::detail {

struct RateDomainSpec {
    std::string name;
    std::uint64_t period_ns = 0;
    std::uint32_t substep_count = 0;
    std::uint64_t relative_deadline_ns = 0;
    std::uint64_t budget_wcet_ns = 0;
    RateCriticality criticality = RateCriticality::normal;
    bool optional = false;
    RateLateAction late_action = RateLateAction::fail;
    std::uint32_t bounded_catch_up_limit = 0;
};

struct RateBindingSpec {
    PhaseHandle phase{};
    RateDomainHandle domain{};
    RatePhaseKind phase_kind = RatePhaseKind::cpu;
};

struct CompiledRatePlan {
    std::uint64_t supercycle_ns = 0;
    std::vector<CompiledRateDomain> domains;
    std::vector<CompiledRateBinding> bindings;
    std::vector<ReferenceRelease> releases;
};

struct RateCompileDiagnostic {
    Status status = Status::ok;
    const char* message = nullptr;
};

// Compiles an epoch-zero, half-open [0, supercycle) reference plan. Output is
// replaced only after complete checked validation and construction succeeds.
[[nodiscard]] Status compile_rate_timeline(
    std::uint32_t graph_owner,
    std::size_t phase_count,
    std::span<const PhaseHandle> compiled_order,
    std::span<const RateDomainSpec> domains,
    std::span<const RateBindingSpec> bindings,
    CompiledRatePlan& output,
    RateCompileDiagnostic& diagnostic) noexcept;

} // namespace rt::detail
