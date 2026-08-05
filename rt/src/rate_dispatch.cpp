#include "rate_dispatch.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace {

bool checked_add(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& output) noexcept {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return false;
    }
    output = left + right;
    return true;
}

bool checked_multiply(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& output) noexcept {
    if (left != 0 &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    output = left * right;
    return true;
}

bool valid_action(rt::RateLateAction action) noexcept {
    return action == rt::RateLateAction::skip ||
        action == rt::RateLateAction::bounded_catch_up ||
        action == rt::RateLateAction::hold ||
        action == rt::RateLateAction::degrade ||
        action == rt::RateLateAction::fail;
}

bool ceil_div(
    std::uint64_t value,
    std::uint64_t divisor,
    std::uint64_t& output) noexcept {
    if (divisor == 0) {
        return false;
    }
    output = value / divisor;
    if (value % divisor != 0) {
        if (output == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }
        ++output;
    }
    return true;
}

} // namespace

namespace rt::detail {

Status compile_rate_dispatch(
    std::uint32_t graph_owner,
    DeterminismTier determinism_tier,
    const RateExecutionPolicy& policy,
    std::span<const GraphDependency> dependencies,
    const CompiledRatePlan& rate_plan,
    std::span<const CrossRateChannelSpec> channels,
    CompiledRateDispatchPlan& output,
    RateDispatchDiagnostic& diagnostic) noexcept {
    diagnostic = {};
    if (policy.maximum_dispatch_records_per_step == 0 ||
        policy.maximum_dispatch_records_per_step >
            reference_release_capacity ||
        policy.host_policy_version == 0 ||
        policy.consecutive_late_threshold == 0 ||
        policy.consecutive_on_time_threshold == 0 ||
        policy.consecutive_late_threshold > rate_policy_threshold_limit ||
        policy.consecutive_on_time_threshold > rate_policy_threshold_limit ||
        policy.rate_telemetry_capacity > rate_telemetry_capacity_limit) {
        diagnostic = {
            Status::invalid_config,
            "active rate execution requires valid positive policy fields and bounded telemetry"};
        return diagnostic.status;
    }
    if (determinism_tier != DeterminismTier::unspecified) {
        diagnostic = {
            Status::invalid_config,
            "active M16-03 rate execution supports only D0 unspecified"};
        return diagnostic.status;
    }
    if (rate_plan.domains.empty() || rate_plan.releases.empty() ||
        rate_plan.supercycle_ns == 0 ||
        rate_plan.domains.size() > rate_domain_capacity) {
        diagnostic = {
            Status::invalid_config,
            "active rate execution requires a finite explicit rate plan"};
        return diagnostic.status;
    }

    try {
        std::array<bool, rate_domain_capacity> producer_domains{};
        for (const auto& channel : channels) {
            if (!channel.producer.valid() ||
                channel.producer.owner() != graph_owner ||
                channel.producer.index() >= rate_plan.bindings.size() ||
                !channel.consumer.valid() ||
                channel.consumer.owner() != graph_owner ||
                channel.consumer.index() >= rate_plan.bindings.size()) {
                diagnostic = {
                    Status::invalid_handle,
                    "active cross-rate endpoint is invalid"};
                return diagnostic.status;
            }
            const auto producer_binding = std::find_if(
                rate_plan.bindings.begin(),
                rate_plan.bindings.end(),
                [&](const CompiledRateBinding& candidate) {
                    return candidate.phase == channel.producer;
                });
            const auto consumer_binding = std::find_if(
                rate_plan.bindings.begin(),
                rate_plan.bindings.end(),
                [&](const CompiledRateBinding& candidate) {
                    return candidate.phase == channel.consumer;
                });
            if (producer_binding == rate_plan.bindings.end() ||
                consumer_binding == rate_plan.bindings.end() ||
                !producer_binding->domain.valid() ||
                !consumer_binding->domain.valid() ||
                producer_binding->domain.index() >= rate_plan.domains.size() ||
                consumer_binding->domain.index() >= rate_plan.domains.size()) {
                diagnostic = {
                    Status::invalid_handle,
                    "active cross-rate endpoint has no valid domain"};
                return diagnostic.status;
            }
            if (rate_plan.domains[producer_binding->domain.index()].optional ||
                rate_plan.domains[consumer_binding->domain.index()].optional) {
                diagnostic = {
                    Status::invalid_config,
                    "optional domains cannot produce or consume active cross-rate channels"};
                return diagnostic.status;
            }
            producer_domains[producer_binding->domain.index()] = true;
        }

        for (std::size_t index = 0;
             index < rate_plan.domains.size();
             ++index) {
            const auto& domain = rate_plan.domains[index];
            if (domain.period_ns == 0 ||
                domain.relative_deadline_ns == 0 ||
                domain.relative_deadline_ns > domain.period_ns ||
                domain.budget_wcet_ns == 0 ||
                !valid_action(domain.late_action) ||
                (domain.late_action == RateLateAction::bounded_catch_up &&
                 domain.bounded_catch_up_limit == 0) ||
                (domain.late_action != RateLateAction::bounded_catch_up &&
                 domain.bounded_catch_up_limit != 0) ||
                (domain.late_action == RateLateAction::skip &&
                 producer_domains[index])) {
                diagnostic = {
                    Status::invalid_config,
                    "active rate domain has invalid deadline, budget, optionality, late action, or catch-up bound"};
                return diagnostic.status;
            }
        }
        for (const auto& binding : rate_plan.bindings) {
            if (binding.phase_kind != RatePhaseKind::cpu) {
                diagnostic = {
                    Status::invalid_config,
                    "active rate execution accepts CPU phases only"};
                return diagnostic.status;
            }
        }

        std::vector<std::size_t> domain_by_phase(
            rate_plan.bindings.size(),
            std::numeric_limits<std::size_t>::max());
        for (const auto& binding : rate_plan.bindings) {
            if (!binding.phase.valid() ||
                binding.phase.owner() != graph_owner ||
                binding.phase.index() >= domain_by_phase.size() ||
                !binding.domain.valid() ||
                binding.domain.owner() != graph_owner ||
                binding.domain.index() >= rate_plan.domains.size()) {
                diagnostic = {
                    Status::invalid_handle,
                    "active rate binding is invalid"};
                return diagnostic.status;
            }
            domain_by_phase[binding.phase.index()] = binding.domain.index();
        }
        for (const auto& dependency : dependencies) {
            if (!dependency.prerequisite.valid() ||
                !dependency.dependent.valid() ||
                dependency.prerequisite.index() >= domain_by_phase.size() ||
                dependency.dependent.index() >= domain_by_phase.size() ||
                domain_by_phase[dependency.prerequisite.index()] !=
                    domain_by_phase[dependency.dependent.index()]) {
                diagnostic = {
                    Status::invalid_config,
                    "active rate execution rejects ordinary cross-domain graph dependencies; use a cross-rate channel"};
                return diagnostic.status;
            }
        }

        CompiledRateDispatchPlan candidate;
        candidate.policy = policy;
        candidate.admission.reserve(rate_plan.releases.size());
        candidate.groups.reserve(rate_plan.releases.size());
        candidate.optional_shed_order.reserve(rate_plan.domains.size());
        for (std::size_t index = 0; index < rate_plan.domains.size(); ++index) {
            if (rate_plan.domains[index].optional) {
                candidate.optional_shed_order.push_back(index);
            }
        }
        std::sort(
            candidate.optional_shed_order.begin(),
            candidate.optional_shed_order.end(),
            [&](std::size_t left, std::size_t right) {
                const auto left_criticality = static_cast<std::uint8_t>(
                    rate_plan.domains[left].criticality);
                const auto right_criticality = static_cast<std::uint8_t>(
                    rate_plan.domains[right].criticality);
                return left_criticality != right_criticality
                    ? left_criticality < right_criticality
                    : left > right;
            });

        std::uint64_t previous_finish = 0;
        for (std::size_t index = 0;
             index < rate_plan.releases.size();
             ++index) {
            const auto& release = rate_plan.releases[index];
            if (release.phase_kind != RatePhaseKind::cpu ||
                release.budget_wcet_ns == 0 ||
                release.relative_deadline_ns == 0 ||
                release.domain_registration_index >=
                    rate_plan.domains.size()) {
                diagnostic = {
                    Status::invalid_config,
                    "active reference record is ambiguous or unsupported",
                    index};
                return diagnostic.status;
            }
            if (!release.optional) {
                const auto declared_start =
                    std::max(previous_finish, release.release_time_ns);
                std::uint64_t declared_finish = 0;
                if (!checked_add(
                        declared_start,
                        release.budget_wcet_ns,
                        declared_finish)) {
                    diagnostic = {
                        Status::capacity_exceeded,
                        "declared-budget admission arithmetic overflowed",
                        index};
                    return diagnostic.status;
                }
                if (declared_finish > release.deadline_time_ns) {
                    diagnostic = {
                        Status::invalid_config,
                        "declared-budget admission found an infeasible mandatory record",
                        index};
                    return diagnostic.status;
                }
                candidate.admission.push_back({
                    index,
                    declared_start,
                    declared_finish,
                    release.deadline_time_ns,
                });
                previous_finish = declared_finish;
            }

            if (candidate.groups.empty() ||
                candidate.groups.back().domain_registration_index !=
                    release.domain_registration_index ||
                candidate.groups.back().domain_release_sequence !=
                    release.domain_release_sequence) {
                candidate.groups.push_back({
                    index,
                    1,
                    release.domain_registration_index,
                    release.domain_release_sequence,
                    release.release_time_ns,
                });
            } else {
                ++candidate.groups.back().reference_count;
            }
        }
        if (previous_finish > rate_plan.supercycle_ns) {
            diagnostic = {
                Status::invalid_config,
                "declared-budget admission is not idle at the supercycle boundary",
                rate_plan.releases.size() - 1};
            return diagnostic.status;
        }
        for (const auto& group : candidate.groups) {
            auto& records = candidate.records_per_domain_release[
                group.domain_registration_index];
            if (records == 0) {
                records = group.reference_count;
            } else if (records != group.reference_count) {
                diagnostic = {
                    Status::invalid_config,
                    "active domain release has an ambiguous record count",
                    group.first_reference_index};
                return diagnostic.status;
            }
        }
        for (const auto& group : candidate.groups) {
            ++candidate.domain_group_offsets[
                group.domain_registration_index + 1];
        }
        for (std::size_t index = 1;
             index < candidate.domain_group_offsets.size();
             ++index) {
            candidate.domain_group_offsets[index] +=
                candidate.domain_group_offsets[index - 1];
        }
        candidate.domain_group_indices.resize(candidate.groups.size());
        auto next_group = candidate.domain_group_offsets;
        for (std::size_t group_index = 0;
             group_index < candidate.groups.size();
             ++group_index) {
            const auto domain = candidate.groups[group_index]
                .domain_registration_index;
            candidate.domain_group_indices[next_group[domain]++] =
                group_index;
        }

        output = std::move(candidate);
        return Status::ok;
    } catch (const std::bad_alloc&) {
        diagnostic = {Status::resource_exhausted, nullptr};
        return diagnostic.status;
    } catch (...) {
        diagnostic = {Status::internal_error, nullptr};
        return diagnostic.status;
    }
}

Status count_due_domain_releases(
    const CompiledRatePlan& rate_plan,
    const CompiledRateDispatchPlan& dispatch_plan,
    std::size_t domain_index,
    std::uint64_t cursor_ns,
    std::uint64_t end_ns,
    std::uint64_t& domain_releases,
    std::uint64_t& reference_records) noexcept {
    domain_releases = 0;
    reference_records = 0;
    if (cursor_ns > end_ns || domain_index >= rate_plan.domains.size()) {
        return Status::invalid_argument;
    }
    const auto period = rate_plan.domains[domain_index].period_ns;
    const auto records =
        dispatch_plan.records_per_domain_release[domain_index];
    std::uint64_t first_sequence = 0;
    std::uint64_t end_sequence = 0;
    if (period == 0 || records == 0 ||
        !ceil_div(cursor_ns, period, first_sequence) ||
        !ceil_div(end_ns, period, end_sequence) ||
        end_sequence < first_sequence) {
        return Status::invalid_argument;
    }
    domain_releases = end_sequence - first_sequence;
    if (!checked_multiply(
            domain_releases,
            static_cast<std::uint64_t>(records),
            reference_records)) {
        domain_releases = 0;
        return Status::capacity_exceeded;
    }
    return Status::ok;
}

Status count_due_rate_work(
    const CompiledRatePlan& rate_plan,
    const CompiledRateDispatchPlan& dispatch_plan,
    std::uint64_t cursor_ns,
    std::uint64_t end_ns,
    RateDueCounts& output) noexcept {
    RateDueCounts candidate;
    if (cursor_ns > end_ns) {
        return Status::invalid_argument;
    }
    for (std::size_t domain = 0;
         domain < rate_plan.domains.size();
         ++domain) {
        std::uint64_t releases = 0;
        std::uint64_t records = 0;
        const auto status = count_due_domain_releases(
            rate_plan,
            dispatch_plan,
            domain,
            cursor_ns,
            end_ns,
            releases,
            records);
        if (status != Status::ok ||
            !checked_add(
                candidate.domain_releases,
                releases,
                candidate.domain_releases) ||
            !checked_add(
                candidate.reference_records,
                records,
                candidate.reference_records)) {
            return Status::capacity_exceeded;
        }
    }
    output = candidate;
    return Status::ok;
}

} // namespace rt::detail
