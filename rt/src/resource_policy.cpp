#include "resource_policy.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <vector>
#include <string_view>

namespace {

constexpr std::uint64_t kThreadAccountingDomain = std::uint64_t{1} << 32u;
constexpr std::uint64_t kMemoryAccountingDomain = std::uint64_t{2} << 32u;

std::size_t owner_index(rt::detail::ControlExtentOwner owner) noexcept {
    return static_cast<std::size_t>(owner);
}

rt::ResourceAccountingExactness combine_exactness(
    rt::ResourceAccountingExactness left,
    rt::ResourceAccountingExactness right) noexcept {
    using Exactness = rt::ResourceAccountingExactness;
    if (left == Exactness::not_applicable) {
        return right;
    }
    if (right == Exactness::not_applicable) {
        return left;
    }
    if (left == Exactness::partial || right == Exactness::partial) {
        return Exactness::partial;
    }
    if (left == Exactness::unknown && right == Exactness::unknown) {
        return Exactness::unknown;
    }
    if (left == Exactness::unknown || right == Exactness::unknown) {
        return Exactness::partial;
    }
    if (left == Exactness::declared_only ||
        right == Exactness::declared_only) {
        return Exactness::declared_only;
    }
    return Exactness::exact;
}

constexpr std::size_t policy_byte_ceiling() noexcept {
    constexpr auto one_tib = std::uint64_t{1} << 40u;
    if constexpr (std::numeric_limits<std::size_t>::max() < one_tib) {
        return std::numeric_limits<std::size_t>::max();
    }
    return static_cast<std::size_t>(one_tib);
}

bool checked_add(
    std::size_t left,
    std::size_t right,
    std::size_t& output) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    output = left + right;
    return true;
}

bool checked_multiply(
    std::size_t left,
    std::size_t right,
    std::size_t& output) noexcept {
    if (left != 0 &&
        right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    output = left * right;
    return true;
}

bool checked_align_up(
    std::size_t value,
    std::size_t alignment,
    std::size_t& output) noexcept {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return false;
    }
    const auto remainder = value & (alignment - 1);
    if (remainder == 0) {
        output = value;
        return true;
    }
    return checked_add(value, alignment - remainder, output);
}

bool valid_thread_role(rt::ThreadRoleId role) noexcept {
    return role == rt::thread_role_frame ||
           role == rt::thread_role_executor_worker ||
           role == rt::thread_role_watchdog ||
           role == rt::thread_role_device_service ||
           role == rt::thread_role_xdma_io ||
           role.value >= rt::thread_role_custom_first;
}

bool valid_memory_region(rt::MemoryRegionId region) noexcept {
    return region.value >= rt::memory_region_runtime_control.value &&
           region.value <= rt::memory_region_host_provider.value;
}

bool valid_policy_name(
    const std::array<char, rt::thread_name_capacity>& name) noexcept {
    const auto terminator = std::find(name.begin(), name.end(), '\0');
    if (terminator == name.end()) {
        return false;
    }
    for (auto cursor = name.begin(); cursor != terminator; ++cursor) {
        const auto value = *cursor;
        const bool valid =
            (value >= 'a' && value <= 'z') ||
            (value >= 'A' && value <= 'Z') ||
            (value >= '0' && value <= '9') ||
            value == '.' || value == '_' || value == '-';
        if (!valid) {
            return false;
        }
    }
    return std::all_of(
        terminator,
        name.end(),
        [](char value) { return value == '\0'; });
}

bool valid_thread_policy(const rt::ThreadPolicy& policy) noexcept {
    switch (policy.requirement) {
    case rt::PolicyRequirement::best_effort:
    case rt::PolicyRequirement::strict:
        break;
    default:
        return false;
    }
    switch (policy.scheduling_class) {
    case rt::SchedulingClass::inherit:
    case rt::SchedulingClass::normal:
        if (policy.scheduling_priority != 0) {
            return false;
        }
        break;
    case rt::SchedulingClass::fifo:
    case rt::SchedulingClass::round_robin:
        if (policy.scheduling_priority < 1 ||
            policy.scheduling_priority > 99) {
            return false;
        }
        break;
    default:
        return false;
    }
    switch (policy.wait_strategy) {
    case rt::WaitStrategy::inherit:
    case rt::WaitStrategy::spin:
    case rt::WaitStrategy::yield:
    case rt::WaitStrategy::park:
        break;
    default:
        return false;
    }
    if (policy.numa_node < -1 ||
        policy.cpu_set.count > rt::cpu_set_capacity ||
        policy.stack_bytes > policy_byte_ceiling() ||
        policy.guard_bytes > policy_byte_ceiling() ||
        (policy.guard_bytes != 0 && policy.stack_bytes == 0) ||
        !valid_policy_name(policy.name)) {
        return false;
    }
    std::size_t stack_commit = 0;
    if (!checked_add(
            policy.stack_bytes,
            policy.guard_bytes,
            stack_commit) ||
        stack_commit > policy_byte_ceiling()) {
        return false;
    }
    for (std::size_t index = 0; index < policy.cpu_set.count; ++index) {
        if (policy.cpu_set.cpu_ids[index] ==
            std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (policy.cpu_set.cpu_ids[earlier] ==
                policy.cpu_set.cpu_ids[index]) {
                return false;
            }
        }
    }
    return true;
}

bool valid_toggle(rt::PolicyToggle toggle) noexcept {
    switch (toggle) {
    case rt::PolicyToggle::inherit:
    case rt::PolicyToggle::disabled:
    case rt::PolicyToggle::enabled:
        return true;
    }
    return false;
}

bool valid_memory_policy(const rt::MemoryPolicy& policy) noexcept {
    switch (policy.requirement) {
    case rt::PolicyRequirement::best_effort:
    case rt::PolicyRequirement::strict:
        break;
    default:
        return false;
    }
    switch (policy.provider) {
    case rt::MemoryProviderOwnership::inherit:
    case rt::MemoryProviderOwnership::runtime:
    case rt::MemoryProviderOwnership::host:
    case rt::MemoryProviderOwnership::backend:
    case rt::MemoryProviderOwnership::borrowed:
        break;
    default:
        return false;
    }
    if (policy.alignment != 0 &&
        (policy.alignment < alignof(std::max_align_t) ||
         policy.alignment > policy_byte_ceiling() ||
         (policy.alignment & (policy.alignment - 1)) != 0)) {
        return false;
    }
    switch (policy.page_rounding) {
    case rt::PageRounding::inherit:
    case rt::PageRounding::none:
    case rt::PageRounding::base_page:
        break;
    default:
        return false;
    }
    if (policy.guard_bytes_before > policy_byte_ceiling() ||
        policy.guard_bytes_after > policy_byte_ceiling()) {
        return false;
    }
    std::size_t guarded_bytes = 0;
    if (!checked_add(
            policy.guard_bytes_before,
            policy.guard_bytes_after,
            guarded_bytes) ||
        guarded_bytes > policy_byte_ceiling()) {
        return false;
    }
    if (!valid_toggle(policy.prefault) ||
        !valid_toggle(policy.locking) ||
        !valid_toggle(policy.pinning) ||
        !valid_toggle(policy.huge_page_fallback) ||
        !valid_toggle(policy.residency_verification)) {
        return false;
    }
    switch (policy.huge_pages) {
    case rt::HugePagePreference::inherit:
    case rt::HugePagePreference::disabled:
    case rt::HugePagePreference::prefer:
        break;
    default:
        return false;
    }
    if (policy.huge_page_fallback == rt::PolicyToggle::enabled &&
        policy.huge_pages != rt::HugePagePreference::prefer) {
        return false;
    }
    if (policy.numa_node < -1 || policy.numa_node > 1023) {
        return false;
    }
    switch (policy.first_touch) {
    case rt::FirstTouchPolicy::inherit:
    case rt::FirstTouchPolicy::none:
    case rt::FirstTouchPolicy::caller:
    case rt::FirstTouchPolicy::owner_thread:
        break;
    default:
        return false;
    }
    switch (policy.rollback) {
    case rt::RollbackIntent::inherit:
    case rt::RollbackIntent::none:
    case rt::RollbackIntent::release:
        break;
    default:
        return false;
    }
    return true;
}

bool memory_policy_is_noop(const rt::MemoryPolicy& policy) noexcept {
    return policy.provider == rt::MemoryProviderOwnership::inherit &&
           policy.alignment == 0 &&
           policy.page_rounding == rt::PageRounding::inherit &&
           policy.guard_bytes_before == 0 &&
           policy.guard_bytes_after == 0 &&
           policy.prefault == rt::PolicyToggle::inherit &&
           policy.locking == rt::PolicyToggle::inherit &&
           policy.pinning == rt::PolicyToggle::inherit &&
           policy.huge_pages == rt::HugePagePreference::inherit &&
           policy.huge_page_fallback == rt::PolicyToggle::inherit &&
           policy.numa_node == -1 &&
           policy.first_touch == rt::FirstTouchPolicy::inherit &&
           policy.residency_verification == rt::PolicyToggle::inherit &&
           policy.rollback == rt::RollbackIntent::inherit;
}

template <std::size_t Capacity>
void set_name(
    std::array<char, Capacity>& output,
    std::string_view value) noexcept {
    output.fill('\0');
    const auto count = std::min(value.size(), output.size() - 1);
    std::copy_n(value.begin(), count, output.begin());
}

void set_custom_thread_name(
    std::array<char, rt::resource_accounting_name_capacity>& output,
    std::uint32_t role) noexcept {
    constexpr std::string_view prefix = "thread.custom.";
    output.fill('\0');
    std::copy(prefix.begin(), prefix.end(), output.begin());
    auto* begin = output.data() + prefix.size();
    auto* end = output.data() + output.size() - 1;
    const auto result = std::to_chars(begin, end, role);
    if (result.ec != std::errc{}) {
        output.fill('\0');
    }
}

rt::ThreadPolicy resolved_thread_policy(
    rt::ThreadRoleId role,
    bool runtime_owned) noexcept {
    rt::ThreadPolicy policy;
    if (role == rt::thread_role_executor_worker && runtime_owned) {
        policy.wait_strategy = rt::WaitStrategy::yield;
    } else if (role == rt::thread_role_watchdog ||
               role == rt::thread_role_device_service) {
        policy.wait_strategy = rt::WaitStrategy::park;
    }
    return policy;
}

rt::MemoryPolicy resolved_memory_policy(
    rt::MemoryRegionId region,
    const rt::MemoryPlan& plan) noexcept {
    rt::MemoryPolicy policy;
    policy.page_rounding = rt::PageRounding::none;
    policy.prefault = rt::PolicyToggle::disabled;
    policy.locking = rt::PolicyToggle::disabled;
    policy.pinning = rt::PolicyToggle::disabled;
    policy.huge_pages = rt::HugePagePreference::disabled;
    policy.huge_page_fallback = rt::PolicyToggle::disabled;
    policy.first_touch = rt::FirstTouchPolicy::none;
    policy.residency_verification = rt::PolicyToggle::disabled;

    if (region == rt::memory_region_registered_state ||
        region == rt::memory_region_registered_device_buffer) {
        policy.provider = rt::MemoryProviderOwnership::borrowed;
        policy.rollback = rt::RollbackIntent::none;
    } else if (region == rt::memory_region_backend_control) {
        policy.provider = rt::MemoryProviderOwnership::backend;
        policy.rollback = rt::RollbackIntent::none;
    } else if (region == rt::memory_region_external_thread_stack ||
               region == rt::memory_region_host_provider) {
        policy.provider = rt::MemoryProviderOwnership::host;
        policy.rollback = rt::RollbackIntent::none;
    } else {
        policy.provider = rt::MemoryProviderOwnership::runtime;
        policy.rollback = rt::RollbackIntent::release;
    }
    if (region == rt::memory_region_phase_scratch ||
        region == rt::memory_region_task_scratch) {
        policy.alignment = plan.scratch_alignment;
    } else if (region == rt::memory_region_trace_storage) {
        policy.alignment = 64;
    }
    return policy;
}

const rt::ThreadPolicyRequest* find_thread_request(
    const rt::CpuMemoryPolicy& policy,
    rt::ThreadRoleId role) noexcept {
    for (std::size_t index = 0;
         index < policy.thread_policy_count;
         ++index) {
        if (policy.thread_policies[index].role == role) {
            return &policy.thread_policies[index];
        }
    }
    return nullptr;
}

const rt::MemoryPolicyRequest* find_memory_request(
    const rt::CpuMemoryPolicy& policy,
    rt::MemoryRegionId region) noexcept {
    for (std::size_t index = 0;
         index < policy.memory_policy_count;
         ++index) {
        if (policy.memory_policies[index].region == region) {
            return &policy.memory_policies[index];
        }
    }
    return nullptr;
}

bool provider_has(
    const rt::MemoryProvider* provider,
    rt::MemoryProviderCapability capability) noexcept {
    return provider &&
        (provider->capabilities &
         rt::memory_provider_capability_bit(capability)) != 0;
}

bool provider_capabilities_support(
    const rt::MemoryProvider* provider,
    const rt::MemoryPolicy& requested) noexcept {
    if (!provider) {
        return false;
    }
    if ((requested.guard_bytes_before != 0 ||
         requested.guard_bytes_after != 0) &&
        !provider_has(provider, rt::MemoryProviderCapability::guard_pages)) {
        return false;
    }
    if (requested.huge_pages == rt::HugePagePreference::prefer &&
        !provider_has(
            provider,
            rt::MemoryProviderCapability::explicit_huge_pages) &&
        requested.huge_page_fallback != rt::PolicyToggle::enabled) {
        return false;
    }
    if (requested.pinning == rt::PolicyToggle::enabled &&
        !provider_has(provider, rt::MemoryProviderCapability::pinning)) {
        return false;
    }
    if (requested.numa_node >= 0 &&
        !provider_has(provider, rt::MemoryProviderCapability::numa_binding)) {
        return false;
    }
    return provider_has(
               provider,
               rt::MemoryProviderCapability::policy_operations) &&
           provider_has(
               provider,
               rt::MemoryProviderCapability::independent_observation);
}

rt::Status finish_memory_report(
    const rt::MemoryPolicyRequest* request,
    const rt::MemoryProvider* provider,
    rt::MemoryPolicyReport& report,
    const char*& diagnostic) noexcept {
    const bool selected =
        report.region == rt::memory_region_phase_scratch ||
        report.region == rt::memory_region_task_scratch ||
        report.region == rt::memory_region_trace_storage;
    const bool runtime_stack =
        report.region == rt::memory_region_runtime_thread_stack;
    const bool active = selected
        ? report.accounted_bytes != 0
        : report.logical_region_count != 0 || report.accounted_bytes != 0;
    if (!request) {
        if (!active) {
            report.resolution = rt::PolicyResolutionState::inactive;
        } else if (selected && provider) {
            report.resolved.provider = rt::MemoryProviderOwnership::host;
            report.resolution = rt::PolicyResolutionState::native_supported;
        }
        return rt::Status::ok;
    }

    report.requested = request->policy;
    if (!active) {
        report.resolution = rt::PolicyResolutionState::inactive;
        if (!memory_policy_is_noop(request->policy) &&
            request->policy.requirement == rt::PolicyRequirement::strict) {
            diagnostic = "strict memory policy targets an inactive region";
            return rt::Status::invalid_config;
        }
        return rt::Status::ok;
    }
    if (runtime_stack) {
        auto resolved = report.resolved;
        resolved.requirement = request->policy.requirement;
        if (request->policy.locking != rt::PolicyToggle::inherit) {
            resolved.locking = request->policy.locking;
        }
        if (request->policy.residency_verification !=
            rt::PolicyToggle::inherit) {
            resolved.residency_verification =
                request->policy.residency_verification;
        }
        const bool unsupported =
            (request->policy.provider !=
                 rt::MemoryProviderOwnership::inherit &&
             request->policy.provider !=
                 rt::MemoryProviderOwnership::runtime) ||
            request->policy.alignment != 0 ||
            (request->policy.page_rounding != rt::PageRounding::inherit &&
             request->policy.page_rounding != rt::PageRounding::none) ||
            request->policy.guard_bytes_before != 0 ||
            request->policy.guard_bytes_after != 0 ||
            request->policy.prefault == rt::PolicyToggle::enabled ||
            request->policy.pinning == rt::PolicyToggle::enabled ||
            request->policy.huge_pages == rt::HugePagePreference::prefer ||
            request->policy.huge_page_fallback == rt::PolicyToggle::enabled ||
            request->policy.numa_node >= 0 ||
            request->policy.first_touch == rt::FirstTouchPolicy::caller ||
            request->policy.first_touch ==
                rt::FirstTouchPolicy::owner_thread ||
            request->policy.rollback == rt::RollbackIntent::none
#if !defined(__linux__)
            || request->policy.locking == rt::PolicyToggle::enabled ||
            request->policy.residency_verification ==
                rt::PolicyToggle::enabled
#endif
            ;
        if (unsupported) {
            report.resolution =
                rt::PolicyResolutionState::unsupported_best_effort;
            if (request->policy.requirement ==
                rt::PolicyRequirement::strict) {
                diagnostic =
                    "strict runtime-stack policy requires unsupported mutation or observation";
                return rt::Status::invalid_config;
            }
            return rt::Status::ok;
        }
        report.resolved = resolved;
        report.resolution = memory_policy_is_noop(request->policy)
            ? rt::PolicyResolutionState::portable_noop
            : rt::PolicyResolutionState::native_supported;
        return rt::Status::ok;
    }
    if (!selected) {
        report.resolution = memory_policy_is_noop(request->policy)
            ? rt::PolicyResolutionState::portable_noop
            : rt::PolicyResolutionState::unsupported_best_effort;
        if (!memory_policy_is_noop(request->policy) &&
            request->policy.requirement == rt::PolicyRequirement::strict) {
            diagnostic = "strict memory policy targets a deferred or borrowed region";
            return rt::Status::invalid_config;
        }
        return rt::Status::ok;
    }

    auto resolved = report.resolved;
    resolved.requirement = request->policy.requirement;
    resolved.provider = provider
        ? rt::MemoryProviderOwnership::host
        : rt::MemoryProviderOwnership::runtime;
    if (request->policy.alignment != 0) {
        resolved.alignment = std::max(
            resolved.alignment,
            request->policy.alignment);
    }
    if (request->policy.page_rounding != rt::PageRounding::inherit) {
        resolved.page_rounding = request->policy.page_rounding;
    }
    resolved.guard_bytes_before = request->policy.guard_bytes_before;
    resolved.guard_bytes_after = request->policy.guard_bytes_after;
    if (request->policy.prefault != rt::PolicyToggle::inherit) {
        resolved.prefault = request->policy.prefault;
    }
    if (request->policy.locking != rt::PolicyToggle::inherit) {
        resolved.locking = request->policy.locking;
    }
    if (request->policy.pinning != rt::PolicyToggle::inherit) {
        resolved.pinning = request->policy.pinning;
    }
    if (request->policy.huge_pages != rt::HugePagePreference::inherit) {
        resolved.huge_pages = request->policy.huge_pages;
    }
    if (request->policy.huge_page_fallback != rt::PolicyToggle::inherit) {
        resolved.huge_page_fallback = request->policy.huge_page_fallback;
    }
    resolved.numa_node = request->policy.numa_node;
    if (request->policy.first_touch != rt::FirstTouchPolicy::inherit) {
        resolved.first_touch = request->policy.first_touch;
    }
    if (request->policy.residency_verification != rt::PolicyToggle::inherit) {
        resolved.residency_verification =
            request->policy.residency_verification;
    }
    resolved.rollback = rt::RollbackIntent::release;

    bool unsupported = false;
    if (request->policy.provider != rt::MemoryProviderOwnership::inherit &&
        request->policy.provider != resolved.provider) {
        unsupported = true;
    }
    if (request->policy.provider == rt::MemoryProviderOwnership::backend ||
        request->policy.provider == rt::MemoryProviderOwnership::borrowed ||
        request->policy.first_touch == rt::FirstTouchPolicy::owner_thread ||
        request->policy.rollback == rt::RollbackIntent::none) {
        unsupported = true;
    }
    if (provider) {
        unsupported = unsupported ||
            !provider_capabilities_support(provider, request->policy);
    } else {
        if (request->policy.pinning == rt::PolicyToggle::enabled ||
            request->policy.numa_node >= 0 ||
            (request->policy.locking == rt::PolicyToggle::enabled &&
             request->policy.requirement == rt::PolicyRequirement::strict)) {
            unsupported = true;
        }
#if !defined(__linux__)
        unsupported = unsupported || !memory_policy_is_noop(request->policy);
#endif
    }
    if (unsupported) {
        if (request->policy.requirement == rt::PolicyRequirement::strict) {
            diagnostic = "strict memory policy capability is unsupported or unobservable";
            return rt::Status::invalid_config;
        }
        report.resolution =
            rt::PolicyResolutionState::native_best_effort_fallback;
        report.resolved = resolved_memory_policy(report.region, rt::MemoryPlan{});
        report.resolved.alignment = resolved.alignment;
        report.resolved.provider = provider
            ? rt::MemoryProviderOwnership::host
            : rt::MemoryProviderOwnership::runtime;
        return rt::Status::ok;
    }
    report.resolved = resolved;
    report.resolution = memory_policy_is_noop(request->policy)
        ? rt::PolicyResolutionState::portable_noop
        : rt::PolicyResolutionState::native_supported;
    return rt::Status::ok;
}

} // namespace

namespace rt::detail {

Status build_cpu_memory_policy_report(
    const CpuMemoryPolicy& policy,
    const RuntimeConfig& config,
    const MemoryPlan& memory_plan,
    std::size_t registered_device_buffer_bytes,
    const MemoryProvider* memory_provider,
    ThreadPolicyProvider& thread_policy_provider,
    CpuMemoryPolicyReport& report,
    const char*& diagnostic) noexcept {
    report = {};
    report.accounting_requirement = policy.accounting_requirement;
    diagnostic = nullptr;
    if (policy.thread_policy_count > thread_policy_request_capacity ||
        policy.memory_policy_count > memory_policy_request_capacity ||
        policy.accounting_declaration_count >
            resource_accounting_declaration_capacity) {
        diagnostic = "CPU/memory policy request capacity is invalid";
        return Status::invalid_config;
    }
    if (policy.accounting_requirement != PolicyRequirement::best_effort &&
        policy.accounting_requirement != PolicyRequirement::strict) {
        diagnostic = "accounting closure requirement is malformed";
        return Status::invalid_config;
    }
    for (std::size_t index = 0;
         index < policy.accounting_declaration_count;
         ++index) {
        const auto& declaration = policy.accounting_declarations[index];
        if (declaration.accounting_key == 0 ||
            declaration.logical_region_count > policy_byte_ceiling() ||
            declaration.accounted_bytes > policy_byte_ceiling()) {
            diagnostic = "accounting declaration is malformed or exceeds bounds";
            return Status::invalid_config;
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (policy.accounting_declarations[earlier].accounting_key ==
                declaration.accounting_key) {
                diagnostic = "accounting declarations contain a duplicate key";
                return Status::invalid_config;
            }
        }
    }
    for (std::size_t index = 0;
         index < policy.thread_policy_count;
         ++index) {
        const auto& request = policy.thread_policies[index];
        if (!valid_thread_role(request.role) ||
            !valid_thread_policy(request.policy)) {
            diagnostic = "thread policy request is malformed or contradictory";
            return Status::invalid_config;
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (policy.thread_policies[earlier].role == request.role) {
                diagnostic = "thread policy contains a duplicate role";
                return Status::invalid_config;
            }
        }
    }
    for (std::size_t index = 0;
         index < policy.memory_policy_count;
         ++index) {
        const auto& request = policy.memory_policies[index];
        if (!valid_memory_region(request.region) ||
            !valid_memory_policy(request.policy)) {
            diagnostic = "memory policy request is malformed or contradictory";
            return Status::invalid_config;
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (policy.memory_policies[earlier].region == request.region) {
                diagnostic = "memory policy contains a duplicate region";
                return Status::invalid_config;
            }
        }
    }

    const bool native_executor =
        config.executor_policy != ExecutorPolicy::host_adapter;
    std::size_t runtime_stack_count = native_executor
        ? config.worker_count
        : 0;
    if (config.watchdog_timeout_ns != 0 &&
        !checked_add(runtime_stack_count, 1, runtime_stack_count)) {
        diagnostic = "runtime thread inventory overflows";
        return Status::invalid_config;
    }
    if (memory_plan.device_backend_count != 0 &&
        !checked_add(runtime_stack_count, 1, runtime_stack_count)) {
        diagnostic = "runtime thread inventory overflows";
        return Status::invalid_config;
    }
    std::size_t external_stack_count = 1;
    bool external_stack_cardinality_known =
        memory_plan.device_backend_count == 0;
    if (!native_executor &&
        !checked_add(
            external_stack_count,
            config.worker_count,
            external_stack_count)) {
        diagnostic = "external thread inventory overflows";
        return Status::invalid_config;
    }
    for (std::size_t index = 0;
         index < policy.thread_policy_count;
         ++index) {
        const auto& request = policy.thread_policies[index];
        std::size_t count = 0;
        bool cardinality_known = true;
        if (request.role == thread_role_frame) {
            count = 1;
        } else if (request.role == thread_role_executor_worker) {
            count = config.worker_count;
        } else if (request.role == thread_role_watchdog) {
            count = config.watchdog_timeout_ns == 0 ? 0 : 1;
        } else if (request.role == thread_role_device_service) {
            count = memory_plan.device_backend_count == 0 ? 0 : 1;
        } else {
            cardinality_known = false;
            external_stack_cardinality_known = false;
        }
        std::size_t per_thread = 0;
        std::size_t total_stack = 0;
        if (cardinality_known &&
            (!checked_add(
                 request.policy.stack_bytes,
                 request.policy.guard_bytes,
                 per_thread) ||
             !checked_multiply(per_thread, count, total_stack) ||
             total_stack > policy_byte_ceiling())) {
            diagnostic = "thread stack policy overflows bounded accounting";
            return Status::invalid_config;
        }
    }

    Status thread_resolution_status = Status::ok;
    const auto add_thread =
        [&](ThreadRoleId role,
            std::string_view name,
            ResourceOwnership ownership,
            PolicyApplicationMode mode,
            std::size_t count,
            bool cardinality_known) {
            auto& row = report.threads[report.thread_count++];
            row.role = role;
            row.accounting_key = {
                kThreadAccountingDomain |
                static_cast<std::uint64_t>(role.value)};
            set_name(row.stable_name, name);
            row.ownership = ownership;
            row.application_mode = mode;
            row.logical_instance_count = count;
            row.cardinality_known = cardinality_known;
            row.accounting_exactness =
                mode == PolicyApplicationMode::apply_and_verify
                ? ResourceAccountingExactness::exact
                : (count == 0 && cardinality_known
                    ? ResourceAccountingExactness::not_applicable
                    : ResourceAccountingExactness::unknown);
            const auto role_default = resolved_thread_policy(
                role,
                mode == PolicyApplicationMode::apply_and_verify);
            const auto* request = find_thread_request(policy, role);
            row.requested = request ? request->policy : ThreadPolicy{};
            thread_resolution_status = thread_policy_provider.resolve(
                role,
                mode,
                count != 0 || !cardinality_known,
                role == thread_role_frame,
                request != nullptr,
                row.requested,
                role_default,
                row.resolved,
                row.resolution,
                row.resolution_error);
        };

    add_thread(
        thread_role_frame,
        "thread.frame",
        ResourceOwnership::caller,
        PolicyApplicationMode::verify_only,
        1,
        true);
    if (thread_resolution_status != Status::ok) {
        diagnostic = "strict frame thread policy is unsupported or unavailable";
        return thread_resolution_status;
    }
    add_thread(
        thread_role_executor_worker,
        "thread.executor-worker",
        native_executor
            ? ResourceOwnership::runtime
            : ResourceOwnership::host_executor,
        native_executor
            ? PolicyApplicationMode::apply_and_verify
            : PolicyApplicationMode::verify_only,
        config.worker_count,
        true);
    if (thread_resolution_status != Status::ok) {
        diagnostic = "strict executor thread policy is unsupported or unavailable";
        return thread_resolution_status;
    }
    add_thread(
        thread_role_watchdog,
        "thread.watchdog",
        ResourceOwnership::runtime,
        PolicyApplicationMode::apply_and_verify,
        config.watchdog_timeout_ns == 0 ? 0 : 1,
        true);
    if (thread_resolution_status != Status::ok) {
        diagnostic = "strict watchdog thread policy is unsupported or inactive";
        return thread_resolution_status;
    }
    add_thread(
        thread_role_device_service,
        "thread.device-service",
        ResourceOwnership::runtime,
        PolicyApplicationMode::apply_and_verify,
        memory_plan.device_backend_count == 0 ? 0 : 1,
        true);
    if (thread_resolution_status != Status::ok) {
        diagnostic = "strict device-service thread policy is unsupported or inactive";
        return thread_resolution_status;
    }
    add_thread(
        thread_role_xdma_io,
        "thread.xdma-io",
        ResourceOwnership::backend,
        PolicyApplicationMode::verify_only,
        0,
        false);
    if (thread_resolution_status != Status::ok) {
        diagnostic = "strict external XDMA thread policy is unsupported";
        return thread_resolution_status;
    }

    for (std::size_t index = 0;
         index < policy.thread_policy_count;
         ++index) {
        const auto& request = policy.thread_policies[index];
        if (request.role.value < thread_role_custom_first) {
            continue;
        }
        auto& row = report.threads[report.thread_count++];
        row.role = request.role;
        row.accounting_key = {
            kThreadAccountingDomain |
            static_cast<std::uint64_t>(request.role.value)};
        set_custom_thread_name(row.stable_name, request.role.value);
        row.ownership = ResourceOwnership::vendor;
        row.application_mode = PolicyApplicationMode::verify_only;
        row.logical_instance_count = 0;
        row.cardinality_known = false;
        row.accounting_exactness = ResourceAccountingExactness::unknown;
        row.requested = request.policy;
        const auto role_default = resolved_thread_policy(request.role, false);
        thread_resolution_status = thread_policy_provider.resolve(
            request.role,
            PolicyApplicationMode::verify_only,
            true,
            false,
            true,
            request.policy,
            role_default,
            row.resolved,
            row.resolution,
            row.resolution_error);
        if (thread_resolution_status != Status::ok) {
            diagnostic = "strict external custom thread policy is unsupported";
            return thread_resolution_status;
        }
    }

    const auto add_memory =
        [&](MemoryRegionId region,
            std::string_view name,
            ResourceOwnership ownership,
            MemoryAccountingScope scope,
            std::size_t count,
            bool cardinality_known,
            std::size_t bytes) {
            auto& row = report.memory[report.memory_count++];
            row.region = region;
            row.accounting_key = {
                kMemoryAccountingDomain |
                static_cast<std::uint64_t>(region.value)};
            set_name(row.stable_name, name);
            row.ownership = ownership;
            row.accounting_scope = scope;
            row.logical_region_count = count;
            row.cardinality_known = cardinality_known;
            row.accounted_bytes = bytes;
            row.resolved = resolved_memory_policy(region, memory_plan);
            const auto status = finish_memory_report(
                find_memory_request(policy, region),
                memory_provider,
                row,
                diagnostic);
            return status;
        };

    if (add_memory(
        memory_region_runtime_control,
        "memory.runtime-control",
        ResourceOwnership::runtime,
        MemoryAccountingScope::planned,
        1,
        true,
        memory_plan.runtime_control_bytes) != Status::ok) return Status::invalid_config;
    if (add_memory(
        memory_region_executor_control,
        "memory.executor-control",
        ResourceOwnership::runtime,
        MemoryAccountingScope::planned,
        1,
        true,
        memory_plan.executor_control_bytes) != Status::ok) return Status::invalid_config;
    if (add_memory(
        memory_region_device_control,
        "memory.device-control",
        ResourceOwnership::runtime,
        MemoryAccountingScope::planned,
        memory_plan.device_backend_count == 0 ? 0 : 1,
        true,
        memory_plan.device_control_bytes) != Status::ok) return Status::invalid_config;
    if (add_memory(
        memory_region_phase_scratch,
        "memory.phase-scratch",
        ResourceOwnership::runtime,
        MemoryAccountingScope::planned,
        memory_plan.phase_count,
        true,
        memory_plan.phase_scratch_total_bytes) != Status::ok) return Status::invalid_config;
    if (add_memory(
        memory_region_task_scratch,
        "memory.task-scratch",
        ResourceOwnership::runtime,
        MemoryAccountingScope::planned,
        memory_plan.task_scratch_slots,
        true,
        memory_plan.task_scratch_total_bytes) != Status::ok) return Status::invalid_config;
    if (add_memory(
        memory_region_trace_storage,
        "memory.trace-storage",
        ResourceOwnership::runtime,
        MemoryAccountingScope::planned,
        memory_plan.trace_capacity,
        true,
        memory_plan.trace_storage_bytes) != Status::ok) return Status::invalid_config;
    if (add_memory(
        memory_region_registered_state,
        "memory.registered-state",
        ResourceOwnership::caller,
        MemoryAccountingScope::informational_external,
        memory_plan.state_count,
        true,
        memory_plan.registered_state_bytes) != Status::ok) return Status::invalid_config;
    if (add_memory(
        memory_region_backend_control,
        "memory.backend-control",
        ResourceOwnership::backend,
        MemoryAccountingScope::informational_external,
        memory_plan.device_backend_count,
        true,
        memory_plan.device_backend_reported_bytes) != Status::ok) return Status::invalid_config;
    if (add_memory(
        memory_region_registered_device_buffer,
        "memory.registered-device-buffer",
        ResourceOwnership::caller,
        MemoryAccountingScope::informational_external,
        memory_plan.device_buffer_count,
        true,
        registered_device_buffer_bytes) != Status::ok) return Status::invalid_config;
    if (add_memory(
        memory_region_runtime_thread_stack,
        "memory.runtime-thread-stack",
        ResourceOwnership::runtime,
        MemoryAccountingScope::excluded,
        runtime_stack_count,
        true,
        0) != Status::ok) return Status::invalid_config;
    if (add_memory(
        memory_region_external_thread_stack,
        "memory.external-thread-stack",
        ResourceOwnership::caller,
        MemoryAccountingScope::excluded,
        external_stack_count,
        external_stack_cardinality_known,
        0) != Status::ok) return Status::invalid_config;
    if (add_memory(
        memory_region_host_provider,
        "memory.host-provider",
        ResourceOwnership::caller,
        MemoryAccountingScope::excluded,
        0,
        true,
        0) != Status::ok) return Status::invalid_config;

    for (std::size_t index = 0; index < report.memory_count; ++index) {
        auto& row = report.memory[index];
        if (row.accounting_scope == MemoryAccountingScope::planned ||
            row.region == memory_region_registered_state ||
            row.region == memory_region_registered_device_buffer) {
            row.accounting_exactness = ResourceAccountingExactness::exact;
        } else if (row.region == memory_region_backend_control) {
            row.accounting_exactness = row.logical_region_count == 0
                ? ResourceAccountingExactness::not_applicable
                : ResourceAccountingExactness::declared_only;
        } else if (row.region == memory_region_runtime_thread_stack) {
            row.accounting_exactness = row.logical_region_count == 0
                ? ResourceAccountingExactness::not_applicable
                : ResourceAccountingExactness::unknown;
        } else if (row.region == memory_region_external_thread_stack) {
            row.accounting_exactness = ResourceAccountingExactness::unknown;
        } else if (row.region == memory_region_host_provider) {
            // Provider commitment and observation remain on the three
            // selected planned rows. This reserved row must stay empty so
            // aggregate totals cannot count the same provider storage twice.
            row.accounting_exactness =
                ResourceAccountingExactness::not_applicable;
        }
    }

    auto find_thread_by_key = [&](std::uint64_t key) -> ThreadPolicyReport* {
        for (std::size_t index = 0; index < report.thread_count; ++index) {
            if (report.threads[index].accounting_key.value == key) {
                return &report.threads[index];
            }
        }
        return nullptr;
    };
    auto find_memory_by_key = [&](std::uint64_t key) -> MemoryPolicyReport* {
        for (std::size_t index = 0; index < report.memory_count; ++index) {
            if (report.memory[index].accounting_key.value == key) {
                return &report.memory[index];
            }
        }
        return nullptr;
    };
    auto* external_stack = find_memory_by_key(
        memory_resource_accounting_key(memory_region_external_thread_stack));
    auto* backend_control = find_memory_by_key(
        memory_resource_accounting_key(memory_region_backend_control));
    if (!external_stack || !backend_control) {
        diagnostic = "memory accounting inventory is incomplete";
        return Status::internal_error;
    }

    bool aggregate_external_stack_declared = false;
    bool any_thread_stack_declared = false;
    std::size_t declared_thread_count = 0;
    std::size_t declared_thread_bytes = 0;
    for (std::size_t index = 0;
         index < policy.accounting_declaration_count;
         ++index) {
        const auto& declaration = policy.accounting_declarations[index];
        if (auto* thread = find_thread_by_key(declaration.accounting_key)) {
            if (thread->application_mode != PolicyApplicationMode::verify_only) {
                diagnostic = "accounting declaration targets a runtime-owned thread";
                return Status::invalid_config;
            }
            if (thread->cardinality_known &&
                declaration.logical_region_count !=
                    thread->logical_instance_count) {
                diagnostic = "thread accounting declaration contradicts known cardinality";
                return Status::invalid_config;
            }
            if (thread->role == thread_role_executor_worker &&
                config.executor_policy != ExecutorPolicy::host_adapter) {
                diagnostic = "executor accounting declaration contradicts runtime ownership";
                return Status::invalid_config;
            }
            thread->logical_instance_count =
                declaration.logical_region_count;
            thread->cardinality_known = true;
            thread->declared_accounted_bytes = declaration.accounted_bytes;
            thread->accounting_exactness =
                ResourceAccountingExactness::declared_only;
            if (!checked_add(
                    declared_thread_count,
                    declaration.logical_region_count,
                    declared_thread_count) ||
                !checked_add(
                    declared_thread_bytes,
                    declaration.accounted_bytes,
                    declared_thread_bytes)) {
                diagnostic = "external thread accounting declaration overflows";
                return Status::invalid_config;
            }
            any_thread_stack_declared = true;
            continue;
        }
        auto* memory = find_memory_by_key(declaration.accounting_key);
        if (!memory ||
            (memory->region != memory_region_backend_control &&
             memory->region != memory_region_external_thread_stack)) {
            diagnostic = "accounting declaration targets an unknown or owned identity";
            return Status::invalid_config;
        }
        if (memory->region == memory_region_backend_control) {
            if (declaration.logical_region_count !=
                    memory_plan.device_backend_count ||
                declaration.accounted_bytes !=
                    memory_plan.device_backend_reported_bytes) {
                diagnostic = "backend accounting declaration contradicts device ABI v1";
                return Status::invalid_config;
            }
        } else if (memory->region == memory_region_external_thread_stack) {
            if ((memory->cardinality_known &&
                 declaration.logical_region_count !=
                     memory->logical_region_count) ||
                (!memory->cardinality_known &&
                 declaration.logical_region_count <
                     memory->logical_region_count)) {
                diagnostic = "external stack declaration contradicts known cardinality";
                return Status::invalid_config;
            }
            aggregate_external_stack_declared = true;
            memory->logical_region_count = declaration.logical_region_count;
            memory->cardinality_known = true;
            memory->accounted_bytes = declaration.accounted_bytes;
            memory->accounting_exactness =
                ResourceAccountingExactness::declared_only;
        }
    }
    if (aggregate_external_stack_declared && any_thread_stack_declared) {
        diagnostic = "external stack accounting mixes aggregate and role declarations";
        return Status::invalid_config;
    }

    if (any_thread_stack_declared) {
        bool all_external_roles_declared = true;
        for (std::size_t index = 0; index < report.thread_count; ++index) {
            const auto& thread = report.threads[index];
            const bool required =
                thread.role == thread_role_frame ||
                (thread.role == thread_role_executor_worker &&
                 config.executor_policy == ExecutorPolicy::host_adapter) ||
                (thread.role == thread_role_xdma_io &&
                 memory_plan.device_backend_count != 0) ||
                thread.role.value >= thread_role_custom_first;
            if (required && thread.accounting_exactness !=
                    ResourceAccountingExactness::declared_only) {
                all_external_roles_declared = false;
            }
        }
        external_stack->logical_region_count = declared_thread_count;
        external_stack->cardinality_known = all_external_roles_declared;
        external_stack->accounted_bytes = declared_thread_bytes;
        external_stack->accounting_exactness = all_external_roles_declared
            ? ResourceAccountingExactness::declared_only
            : ResourceAccountingExactness::partial;
    }

    std::size_t planned_sum = 0;
    std::size_t closed_sum = 0;
    for (std::size_t index = 0; index < report.memory_count; ++index) {
        const auto& row = report.memory[index];
        const auto* request = find_memory_request(policy, row.region);
        if (request) {
            std::size_t guarded = 0;
            std::size_t total = 0;
            if (!checked_add(
                    request->policy.guard_bytes_before,
                    request->policy.guard_bytes_after,
                    guarded) ||
                !checked_add(row.accounted_bytes, guarded, total)) {
                diagnostic = "memory policy region size overflows";
                return Status::invalid_config;
            }
            const auto rounding =
                request->policy.page_rounding == PageRounding::base_page
                ? std::size_t{4096}
                : request->policy.alignment;
            if (rounding != 0 &&
                !checked_align_up(total, rounding, total)) {
                diagnostic = "memory policy page rounding overflows";
                return Status::invalid_config;
            }
            if (total > policy_byte_ceiling()) {
                diagnostic = "memory policy exceeds bounded accounting";
                return Status::invalid_config;
            }
        }
        if (row.accounting_scope == MemoryAccountingScope::planned &&
            !checked_add(planned_sum, row.accounted_bytes, planned_sum)) {
            diagnostic = "memory policy accounting overflows";
            return Status::invalid_config;
        }
        if (!checked_add(closed_sum, row.accounted_bytes, closed_sum)) {
            diagnostic = "cross-category memory accounting overflows";
            return Status::invalid_config;
        }
    }
    if (planned_sum != memory_plan.planned_bytes) {
        diagnostic = "memory policy accounting does not match finalized plan";
        return Status::invalid_config;
    }
    refresh_accounting_totals(report);
    if (policy.accounting_requirement == PolicyRequirement::strict) {
        const auto closure_status = validate_accounting_closure(
            report,
            false,
            diagnostic);
        if (closure_status != Status::ok) {
            return closure_status;
        }
    }
    return Status::ok;
}

Status validate_control_extent_ledger(
    std::span<const LogicalControlExtent> extents,
    const std::array<ControlExtentExpectation, 3>& expected,
    ControlExtentLedger& ledger,
    const char*& diagnostic) noexcept {
    ledger = {};
    diagnostic = nullptr;
    try {
        std::vector<LogicalControlExtent> ordered(
            extents.begin(),
            extents.end());
        for (const auto& extent : ordered) {
            const auto owner = owner_index(extent.owner);
            if (owner >= ledger.extent_counts.size() ||
                extent.stable_extent_id == 0 ||
                extent.data == nullptr || extent.bytes == 0) {
                diagnostic = "control extent is missing or malformed";
                return Status::invalid_config;
            }
            const auto begin = reinterpret_cast<std::uintptr_t>(extent.data);
            if (extent.bytes >
                std::numeric_limits<std::uintptr_t>::max() - begin) {
                diagnostic = "control extent address overflows";
                return Status::invalid_config;
            }
            if (!checked_add(
                    ledger.extent_counts[owner],
                    1,
                    ledger.extent_counts[owner]) ||
                !checked_add(
                    ledger.accounted_bytes[owner],
                    extent.bytes,
                    ledger.accounted_bytes[owner])) {
                diagnostic = "control extent ledger total overflows";
                return Status::invalid_config;
            }
        }
        std::sort(
            ordered.begin(),
            ordered.end(),
            [](const LogicalControlExtent& left,
               const LogicalControlExtent& right) {
                return left.stable_extent_id < right.stable_extent_id;
            });
        for (std::size_t index = 1; index < ordered.size(); ++index) {
            if (ordered[index - 1].stable_extent_id ==
                ordered[index].stable_extent_id) {
                diagnostic = "control extent ledger contains a duplicate identity";
                return Status::invalid_config;
            }
        }
        std::sort(
            ordered.begin(),
            ordered.end(),
            [](const LogicalControlExtent& left,
               const LogicalControlExtent& right) {
                return reinterpret_cast<std::uintptr_t>(left.data) <
                    reinterpret_cast<std::uintptr_t>(right.data);
            });
        for (std::size_t index = 1; index < ordered.size(); ++index) {
            const auto previous_begin = reinterpret_cast<std::uintptr_t>(
                ordered[index - 1].data);
            const auto previous_end = previous_begin + ordered[index - 1].bytes;
            const auto current_begin = reinterpret_cast<std::uintptr_t>(
                ordered[index].data);
            if (current_begin < previous_end) {
                diagnostic = "control extent ledger contains overlapping storage";
                return Status::invalid_config;
            }
        }
    } catch (const std::bad_alloc&) {
        diagnostic = "control extent ledger allocation failed";
        return Status::resource_exhausted;
    } catch (...) {
        diagnostic = "control extent ledger validation failed";
        return Status::internal_error;
    }
    for (std::size_t owner = 0; owner < expected.size(); ++owner) {
        if (ledger.extent_counts[owner] != expected[owner].extent_count) {
            diagnostic = "control extent ledger is missing an expected extent";
            return Status::invalid_config;
        }
        if (ledger.accounted_bytes[owner] !=
            expected[owner].accounted_bytes) {
            diagnostic = "control extent ledger does not match the construction estimate";
            return Status::invalid_config;
        }
    }
    return Status::ok;
}

void refresh_accounting_totals(CpuMemoryPolicyReport& report) noexcept {
    report.planned_total = {};
    report.informational_total = {};
    report.excluded_total = {};
    report.closed_total = {};
    const auto add = [](MemoryAccountingTotal& total,
                        const MemoryPolicyReport& row) {
        std::size_t bytes = 0;
        if (!checked_add(total.accounted_bytes, row.accounted_bytes, bytes)) {
            total.accounted_bytes = 0;
            total.exactness = ResourceAccountingExactness::unknown;
            return;
        }
        total.accounted_bytes = bytes;
        total.exactness = combine_exactness(
            total.exactness,
            row.accounting_exactness);
    };
    for (std::size_t index = 0; index < report.memory_count; ++index) {
        const auto& row = report.memory[index];
        if (row.accounting_scope == MemoryAccountingScope::planned) {
            add(report.planned_total, row);
        } else if (row.accounting_scope ==
                   MemoryAccountingScope::informational_external) {
            add(report.informational_total, row);
        } else {
            add(report.excluded_total, row);
        }
        add(report.closed_total, row);
    }
    report.accounting_complete =
        report.closed_total.exactness == ResourceAccountingExactness::exact ||
        report.closed_total.exactness ==
            ResourceAccountingExactness::declared_only ||
        report.closed_total.exactness ==
            ResourceAccountingExactness::not_applicable;
}

Status validate_accounting_closure(
    const CpuMemoryPolicyReport& report,
    bool require_runtime_stack,
    const char*& diagnostic) noexcept {
    diagnostic = nullptr;
    for (std::size_t index = 0; index < report.memory_count; ++index) {
        const auto& row = report.memory[index];
        if (!require_runtime_stack &&
            row.region == memory_region_runtime_thread_stack) {
            continue;
        }
        if (row.accounting_exactness ==
                ResourceAccountingExactness::unknown ||
            row.accounting_exactness ==
                ResourceAccountingExactness::partial) {
            diagnostic = row.region == memory_region_runtime_thread_stack
                ? "strict accounting closure requires live runtime stack facts"
                : "strict accounting closure requires external declarations";
            return Status::invalid_config;
        }
    }
    return Status::ok;
}

} // namespace rt::detail
