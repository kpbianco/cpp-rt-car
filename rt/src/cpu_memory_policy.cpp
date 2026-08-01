#include "cpu_memory_policy.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace {

constexpr std::size_t kMaximumPolicyAlignment = std::size_t{1} << 30;
constexpr std::size_t kPortablePageBytes = 4096;
constexpr std::uint64_t kMemoryAccountingDomain =
    std::uint64_t{1} << 63;

bool checked_add(
    std::size_t left,
    std::size_t right,
    std::size_t& output) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        output = 0;
        return false;
    }
    output = left + right;
    return true;
}

bool checked_align_up(
    std::size_t value,
    std::size_t alignment,
    std::size_t& output) noexcept {
    if (alignment == 0 ||
        (alignment & (alignment - 1)) != 0) {
        output = 0;
        return false;
    }
    const auto mask = alignment - 1;
    if (value > std::numeric_limits<std::size_t>::max() - mask) {
        output = 0;
        return false;
    }
    output = (value + mask) & ~mask;
    return true;
}

bool identifier_character(char value) noexcept {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') ||
           value == '.' || value == '_' || value == ':' ||
           value == '/' || value == '@' || value == '-';
}

bool valid_optional_name(
    const std::array<char, rt::policy_thread_name_capacity>& name) noexcept {
    if (name.front() == '\0') {
        return std::all_of(
            name.begin(),
            name.end(),
            [](char value) { return value == '\0'; });
    }
    std::size_t length = 0;
    while (length < name.size() && name[length] != '\0') {
        if (!identifier_character(name[length])) {
            return false;
        }
        ++length;
    }
    if (length == name.size()) {
        return false;
    }
    return std::all_of(
        name.begin() + static_cast<std::ptrdiff_t>(length + 1),
        name.end(),
        [](char value) { return value == '\0'; });
}

bool valid_role(rt::ThreadRole role) noexcept {
    switch (role) {
    case rt::ThreadRole::frame:
    case rt::ThreadRole::executor_worker:
    case rt::ThreadRole::watchdog_service:
    case rt::ThreadRole::device_service:
    case rt::ThreadRole::xdma_io:
    case rt::ThreadRole::accelerator_submission:
        return true;
    case rt::ThreadRole::none:
        return false;
    }
    return false;
}

bool valid_requirement(rt::PolicyRequirement requirement) noexcept {
    return requirement == rt::PolicyRequirement::best_effort ||
           requirement == rt::PolicyRequirement::required;
}

bool valid_thread_policy(const rt::ThreadPolicy& policy) noexcept {
    if (!valid_requirement(policy.requirement) ||
        policy.numa_node < -1 ||
        !valid_optional_name(policy.name)) {
        return false;
    }
    switch (policy.scheduling_class) {
    case rt::SchedulingClass::inherit:
        if (policy.scheduling_priority != 0) {
            return false;
        }
        break;
    case rt::SchedulingClass::normal:
        if (policy.scheduling_priority != 0) {
            return false;
        }
        break;
    case rt::SchedulingClass::fifo:
    case rt::SchedulingClass::round_robin:
        if (policy.scheduling_priority == 0) {
            return false;
        }
        break;
    default:
        return false;
    }
    switch (policy.wait_strategy) {
    case rt::WaitStrategy::runtime_default:
    case rt::WaitStrategy::spin:
    case rt::WaitStrategy::yield:
    case rt::WaitStrategy::park:
    case rt::WaitStrategy::adaptive:
        break;
    default:
        return false;
    }
    if (!policy.cpu_set.specified) {
        if (policy.cpu_set.logical_cpu_count != 0 ||
            std::any_of(
                policy.cpu_set.words.begin(),
                policy.cpu_set.words.end(),
                [](std::uint64_t word) { return word != 0; })) {
            return false;
        }
    } else {
        if (policy.cpu_set.logical_cpu_count == 0 ||
            policy.cpu_set.logical_cpu_count >
                rt::policy_cpu_capacity) {
            return false;
        }
        bool any = false;
        for (std::size_t cpu = 0;
             cpu < rt::policy_cpu_capacity;
             ++cpu) {
            const bool set =
                (policy.cpu_set.words[cpu / 64] &
                 (std::uint64_t{1} << (cpu % 64))) != 0;
            if (cpu >= policy.cpu_set.logical_cpu_count && set) {
                return false;
            }
            any = any || set;
        }
        if (!any) {
            return false;
        }
    }
    if (policy.stack_bytes == 0 && policy.guard_bytes != 0) {
        return false;
    }
    std::size_t stack_footprint = 0;
    return checked_add(
        policy.stack_bytes,
        policy.guard_bytes,
        stack_footprint);
}

bool thread_policy_is_default(const rt::ThreadPolicy& policy) noexcept {
    const rt::ThreadPolicy defaults{};
    return policy.requirement == defaults.requirement &&
           policy.cpu_set.specified == defaults.cpu_set.specified &&
           policy.cpu_set.logical_cpu_count ==
               defaults.cpu_set.logical_cpu_count &&
           policy.cpu_set.words == defaults.cpu_set.words &&
           policy.scheduling_class == defaults.scheduling_class &&
           policy.scheduling_priority == defaults.scheduling_priority &&
           policy.numa_node == defaults.numa_node &&
           policy.wait_strategy == defaults.wait_strategy &&
           policy.stack_bytes == defaults.stack_bytes &&
           policy.guard_bytes == defaults.guard_bytes &&
           policy.name == defaults.name;
}

bool valid_memory_category(rt::MemoryCategory category) noexcept {
    switch (category) {
    case rt::MemoryCategory::runtime_control:
    case rt::MemoryCategory::executor_control_and_queues:
    case rt::MemoryCategory::device_control_and_queues:
    case rt::MemoryCategory::phase_scratch:
    case rt::MemoryCategory::task_scratch:
    case rt::MemoryCategory::trace_storage:
    case rt::MemoryCategory::thread_stack:
    case rt::MemoryCategory::backend_storage:
    case rt::MemoryCategory::registered_state:
    case rt::MemoryCategory::registered_device_buffer:
        return true;
    }
    return false;
}

bool valid_memory_id(const rt::MemoryRegionId& id) noexcept {
    if (!valid_memory_category(id.category)) {
        return false;
    }
    if (id.category == rt::MemoryCategory::thread_stack) {
        return valid_role(id.thread_role);
    }
    return id.thread_role == rt::ThreadRole::none;
}

bool valid_toggle(rt::MemoryPolicyToggle value) noexcept {
    switch (value) {
    case rt::MemoryPolicyToggle::runtime_default:
    case rt::MemoryPolicyToggle::disabled:
    case rt::MemoryPolicyToggle::enabled:
        return true;
    }
    return false;
}

bool valid_memory_policy(const rt::MemoryRegionPolicy& policy) noexcept {
    if (!valid_requirement(policy.requirement) ||
        policy.numa_node < -1 ||
        (policy.alignment != 0 &&
         ((policy.alignment & (policy.alignment - 1)) != 0 ||
          policy.alignment > kMaximumPolicyAlignment)) ||
        !valid_toggle(policy.page_rounding) ||
        !valid_toggle(policy.prefault) ||
        !valid_toggle(policy.locking) ||
        !valid_toggle(policy.pinning) ||
        !valid_toggle(policy.residency_verification)) {
        return false;
    }
    switch (policy.provider) {
    case rt::MemoryProviderOwnership::inherit:
    case rt::MemoryProviderOwnership::runtime:
    case rt::MemoryProviderOwnership::host:
    case rt::MemoryProviderOwnership::backend:
        break;
    default:
        return false;
    }
    switch (policy.huge_pages) {
    case rt::HugePagePolicy::runtime_default:
    case rt::HugePagePolicy::disabled:
    case rt::HugePagePolicy::prefer:
    case rt::HugePagePolicy::require:
        break;
    default:
        return false;
    }
    switch (policy.first_touch) {
    case rt::FirstTouchPolicy::runtime_default:
    case rt::FirstTouchPolicy::disabled:
    case rt::FirstTouchPolicy::frame_thread:
    case rt::FirstTouchPolicy::owner_thread:
        break;
    default:
        return false;
    }
    switch (policy.rollback) {
    case rt::RollbackIntent::runtime_default:
    case rt::RollbackIntent::release:
    case rt::RollbackIntent::return_to_provider:
    case rt::RollbackIntent::retain_external:
        break;
    default:
        return false;
    }
    if ((policy.guard_before_bytes != 0 ||
         policy.guard_after_bytes != 0) &&
        policy.page_rounding != rt::MemoryPolicyToggle::enabled) {
        return false;
    }
    if (policy.allow_huge_page_fallback &&
        policy.huge_pages != rt::HugePagePolicy::prefer) {
        return false;
    }
    return true;
}

bool memory_policy_is_default(
    const rt::MemoryRegionPolicy& policy) noexcept {
    const rt::MemoryRegionPolicy defaults{};
    return policy.requirement == defaults.requirement &&
           policy.provider == defaults.provider &&
           policy.alignment == defaults.alignment &&
           policy.page_rounding == defaults.page_rounding &&
           policy.guard_before_bytes == defaults.guard_before_bytes &&
           policy.guard_after_bytes == defaults.guard_after_bytes &&
           policy.prefault == defaults.prefault &&
           policy.locking == defaults.locking &&
           policy.pinning == defaults.pinning &&
           policy.huge_pages == defaults.huge_pages &&
           policy.allow_huge_page_fallback ==
               defaults.allow_huge_page_fallback &&
           policy.numa_node == defaults.numa_node &&
           policy.first_touch == defaults.first_touch &&
           policy.residency_verification ==
               defaults.residency_verification &&
           policy.rollback == defaults.rollback;
}

std::uint64_t thread_accounting_key(
    const rt::ThreadResourceId& id) noexcept {
    return (static_cast<std::uint64_t>(id.role) << 32) |
           static_cast<std::uint64_t>(id.instance);
}

std::uint64_t memory_accounting_key(
    const rt::MemoryRegionId& id) noexcept {
    return kMemoryAccountingDomain |
           (static_cast<std::uint64_t>(id.category) << 48) |
           (static_cast<std::uint64_t>(id.thread_role) << 32) |
           static_cast<std::uint64_t>(id.instance);
}

const rt::ThreadPolicyRequest* find_thread_request(
    std::span<const rt::ThreadPolicyRequest> requests,
    const rt::ThreadResourceId& id) noexcept {
    const auto found = std::find_if(
        requests.begin(),
        requests.end(),
        [&](const auto& request) { return request.id == id; });
    return found == requests.end() ? nullptr : &*found;
}

const rt::MemoryPolicyRequest* find_memory_request(
    std::span<const rt::MemoryPolicyRequest> requests,
    const rt::MemoryRegionId& id) noexcept {
    const auto found = std::find_if(
        requests.begin(),
        requests.end(),
        [&](const auto& request) { return request.id == id; });
    return found == requests.end() ? nullptr : &*found;
}

} // namespace

namespace rt::detail {

Status resolve_thread_policies(
    std::span<const ThreadPolicyRequest> requests,
    std::span<const ThreadInventoryEntry> inventory,
    std::vector<ThreadPolicyReport>& reports,
    const char*& error) noexcept {
    error = nullptr;
    reports.clear();
    if (requests.size() > policy_request_capacity) {
        error = "thread policy request capacity exceeded";
        return Status::invalid_config;
    }
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const auto& request = requests[index];
        if (!valid_role(request.id.role) ||
            !valid_thread_policy(request.policy)) {
            error = "thread policy request is malformed or contradictory";
            return Status::invalid_config;
        }
        if (request.policy.requirement == PolicyRequirement::required) {
            error = "required thread policy is unsupported before native application";
            return Status::invalid_config;
        }
        if (std::find_if(
                requests.begin(),
                requests.begin() + static_cast<std::ptrdiff_t>(index),
                [&](const auto& prior) {
                    return prior.id == request.id;
                }) != requests.begin() +
                    static_cast<std::ptrdiff_t>(index)) {
            error = "duplicate thread-role policy request";
            return Status::invalid_config;
        }
        const bool in_inventory = std::any_of(
            inventory.begin(),
            inventory.end(),
            [&](const auto& entry) { return entry.id == request.id; });
        const bool external_extension =
            request.id.role == ThreadRole::xdma_io ||
            request.id.role == ThreadRole::accelerator_submission;
        if (!in_inventory && !external_extension) {
            error = "thread policy request does not match the finalized role inventory";
            return Status::invalid_config;
        }
    }

    try {
        reports.reserve(inventory.size() + requests.size());
        const auto append =
            [&](ThreadResourceId id, ThreadOwnership ownership) {
                const auto* request = find_thread_request(requests, id);
                ThreadPolicyReport report{};
                report.id = id;
                report.accounting_key = thread_accounting_key(id);
                report.ownership = ownership;
                report.explicitly_requested = request != nullptr;
                report.requested = request ? request->policy : ThreadPolicy{};
                report.resolved = ThreadPolicy{};
                report.resolution =
                    request && !thread_policy_is_default(request->policy)
                    ? PolicyStageState::portable_fallback
                    : PolicyStageState::portable_default;
                report.application = PolicyStageState::not_performed;
                report.verification =
                    ownership == ThreadOwnership::runtime
                    ? PolicyStageState::not_performed
                    : PolicyStageState::verify_only;
                reports.push_back(report);
            };
        for (const auto& entry : inventory) {
            append(entry.id, entry.ownership);
        }
        for (const auto& request : requests) {
            const bool already_present = std::any_of(
                inventory.begin(),
                inventory.end(),
                [&](const auto& entry) {
                    return entry.id == request.id;
                });
            if (!already_present) {
                append(request.id, ThreadOwnership::backend);
            }
        }
    } catch (const std::bad_alloc&) {
        error = "thread policy report allocation failed";
        return Status::resource_exhausted;
    } catch (...) {
        error = "thread policy report construction failed";
        return Status::internal_error;
    }
    return Status::ok;
}

Status resolve_memory_policies(
    std::span<const MemoryPolicyRequest> requests,
    std::span<const MemoryInventoryEntry> inventory,
    std::vector<MemoryRegionPolicyReport>& reports,
    CpuMemoryPolicySummary& summary,
    const char*& error) noexcept {
    error = nullptr;
    reports.clear();
    summary.memory_region_count = 0;
    summary.runtime_accounted_bytes = 0;
    summary.informational_excluded_bytes = 0;
    if (requests.size() > policy_request_capacity) {
        error = "memory policy request capacity exceeded";
        return Status::invalid_config;
    }
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const auto& request = requests[index];
        if (!valid_memory_id(request.id) ||
            !valid_memory_policy(request.policy)) {
            error = "memory policy request is malformed or contradictory";
            return Status::invalid_config;
        }
        if (request.policy.requirement == PolicyRequirement::required) {
            error = "required memory policy is unsupported before native application";
            return Status::invalid_config;
        }
        if (std::find_if(
                requests.begin(),
                requests.begin() + static_cast<std::ptrdiff_t>(index),
                [&](const auto& prior) {
                    return prior.id == request.id;
                }) != requests.begin() +
                    static_cast<std::ptrdiff_t>(index)) {
            error = "duplicate memory-region policy request";
            return Status::invalid_config;
        }
        const auto found = std::find_if(
            inventory.begin(),
            inventory.end(),
            [&](const auto& entry) { return entry.id == request.id; });
        if (found == inventory.end()) {
            error = "memory policy request does not match the finalized region inventory";
            return Status::invalid_config;
        }
        if (request.policy.provider !=
                MemoryProviderOwnership::inherit &&
            request.policy.provider != found->ownership) {
            error = "memory provider ownership contradicts the region inventory";
            return Status::invalid_config;
        }
        if (request.policy.first_touch ==
                FirstTouchPolicy::owner_thread &&
            request.id.category != MemoryCategory::thread_stack) {
            error = "owner-thread first touch requires a thread-stack region";
            return Status::invalid_config;
        }
        if (request.policy.rollback ==
                RollbackIntent::retain_external &&
            found->ownership == MemoryProviderOwnership::runtime) {
            error = "runtime-owned memory cannot use retain-external rollback";
            return Status::invalid_config;
        }
        if (request.policy.rollback ==
                RollbackIntent::return_to_provider &&
            found->ownership == MemoryProviderOwnership::runtime) {
            error = "runtime-owned memory has no external provider return path";
            return Status::invalid_config;
        }
    }

    try {
        reports.reserve(inventory.size());
        for (const auto& entry : inventory) {
            const auto* request = find_memory_request(requests, entry.id);
            MemoryRegionPolicyReport report{};
            report.id = entry.id;
            report.accounting_key = memory_accounting_key(entry.id);
            report.ownership = entry.ownership;
            report.accounting_scope = entry.accounting_scope;
            report.explicitly_requested = request != nullptr;
            report.reported_bytes = entry.reported_bytes;
            report.accounted_bytes = entry.accounted_bytes;
            report.requested =
                request ? request->policy : MemoryRegionPolicy{};
            report.resolved = MemoryRegionPolicy{};
            report.resolved.provider = entry.ownership;
            report.resolution =
                request && !memory_policy_is_default(request->policy)
                ? PolicyStageState::portable_fallback
                : PolicyStageState::portable_default;
            report.application = PolicyStageState::not_performed;
            report.verification =
                entry.ownership == MemoryProviderOwnership::runtime
                ? PolicyStageState::not_performed
                : PolicyStageState::verify_only;

            std::size_t footprint = entry.reported_bytes;
            if (report.requested.alignment != 0 &&
                !checked_align_up(
                    footprint,
                    report.requested.alignment,
                    footprint)) {
                error = "memory policy alignment calculation overflows";
                return Status::invalid_config;
            }
            if (report.requested.page_rounding ==
                    MemoryPolicyToggle::enabled &&
                !checked_align_up(
                    footprint,
                    kPortablePageBytes,
                    footprint)) {
                error = "memory policy page-rounding calculation overflows";
                return Status::invalid_config;
            }
            if (!checked_add(
                    footprint,
                    report.requested.guard_before_bytes,
                    footprint) ||
                !checked_add(
                    footprint,
                    report.requested.guard_after_bytes,
                    footprint)) {
                error = "memory policy guard calculation overflows";
                return Status::invalid_config;
            }
            report.requested_footprint_bytes = footprint;

            auto& total =
                entry.accounting_scope ==
                    MemoryAccountingScope::runtime_plan
                ? summary.runtime_accounted_bytes
                : summary.informational_excluded_bytes;
            const auto contribution =
                entry.accounting_scope ==
                    MemoryAccountingScope::runtime_plan
                ? entry.accounted_bytes
                : entry.reported_bytes;
            if (!checked_add(total, contribution, total)) {
                error = "policy inventory accounting overflows";
                return Status::invalid_config;
            }
            reports.push_back(report);
        }
    } catch (const std::bad_alloc&) {
        error = "memory policy report allocation failed";
        return Status::resource_exhausted;
    } catch (...) {
        error = "memory policy report construction failed";
        return Status::internal_error;
    }
    summary.memory_region_count = reports.size();
    return Status::ok;
}

} // namespace rt::detail
