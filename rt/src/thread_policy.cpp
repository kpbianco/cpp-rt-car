#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "thread_policy.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>

#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#endif

namespace {

#if defined(ENOTSUP)
constexpr std::int32_t kUnsupportedError = ENOTSUP;
#else
constexpr std::int32_t kUnsupportedError = EINVAL;
#endif

#if !defined(__linux__)
bool nondefault_request(const rt::ThreadPolicy& policy) noexcept {
    return policy.cpu_set.count != 0 ||
           policy.scheduling_class != rt::SchedulingClass::inherit ||
           policy.scheduling_priority != 0 ||
           policy.numa_node != -1 ||
           policy.wait_strategy != rt::WaitStrategy::inherit ||
           policy.stack_bytes != 0 || policy.guard_bytes != 0 ||
           policy.name.front() != '\0';
}
#endif

std::size_t policy_name_length(
    const std::array<char, rt::thread_name_capacity>& name) noexcept {
    const auto end = std::find(name.begin(), name.end(), '\0');
    return static_cast<std::size_t>(end - name.begin());
}

void copy_name(
    std::array<char, rt::thread_name_capacity>& output,
    const char* value) noexcept {
    output.fill('\0');
    if (!value) {
        return;
    }
    const auto length = std::min(
        std::strlen(value),
        output.size() - 1);
    std::copy_n(value, length, output.begin());
}

#if defined(__linux__)

rt::SchedulingClass scheduling_class(int policy) noexcept {
    switch (policy) {
    case SCHED_FIFO:
        return rt::SchedulingClass::fifo;
    case SCHED_RR:
        return rt::SchedulingClass::round_robin;
    default:
        return rt::SchedulingClass::normal;
    }
}

int native_scheduling_class(rt::SchedulingClass policy) noexcept {
    switch (policy) {
    case rt::SchedulingClass::normal:
        return SCHED_OTHER;
    case rt::SchedulingClass::fifo:
        return SCHED_FIFO;
    case rt::SchedulingClass::round_robin:
        return SCHED_RR;
    case rt::SchedulingClass::inherit:
        break;
    }
    return SCHED_OTHER;
}

bool cpu_set_to_policy(
    const cpu_set_t& source,
    rt::CpuSetRequest& output) noexcept {
    output = {};
    for (std::size_t cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(static_cast<int>(cpu), &source)) {
            continue;
        }
        if (output.count == output.cpu_ids.size()) {
            output = {};
            return false;
        }
        output.cpu_ids[output.count++] = static_cast<std::uint32_t>(cpu);
    }
    return output.count != 0;
}

bool policy_to_cpu_set(
    const rt::CpuSetRequest& source,
    cpu_set_t& output) noexcept {
    CPU_ZERO(&output);
    if (source.count == 0) {
        return false;
    }
    for (std::size_t index = 0; index < source.count; ++index) {
        const auto cpu = source.cpu_ids[index];
        if (cpu >= CPU_SETSIZE) {
            CPU_ZERO(&output);
            return false;
        }
        CPU_SET(static_cast<int>(cpu), &output);
    }
    return true;
}

bool parse_cpu_number(
    const char*& cursor,
    const char* end,
    std::uint32_t& output) noexcept {
    if (cursor == end || *cursor < '0' || *cursor > '9') {
        return false;
    }
    std::uint64_t value = 0;
    while (cursor != end && *cursor >= '0' && *cursor <= '9') {
        value = (value * 10u) + static_cast<unsigned>(*cursor - '0');
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        ++cursor;
    }
    output = static_cast<std::uint32_t>(value);
    return true;
}

bool read_numa_node_cpus(
    std::int32_t node,
    cpu_set_t& output,
    std::int32_t& system_error) noexcept {
    CPU_ZERO(&output);
    char path[96]{};
    const auto written = std::snprintf(
        path,
        sizeof(path),
        "/sys/devices/system/node/node%d/cpulist",
        node);
    if (written <= 0 || static_cast<std::size_t>(written) >= sizeof(path)) {
        system_error = ENAMETOOLONG;
        return false;
    }
    auto* file = std::fopen(path, "rb");
    if (!file) {
        system_error = errno == 0 ? ENOENT : errno;
        return false;
    }
    char text[4096]{};
    const auto bytes = std::fread(text, 1, sizeof(text) - 1, file);
    const bool complete = std::feof(file) != 0;
    const int close_status = std::fclose(file);
    if (!complete || close_status != 0 || bytes == 0) {
        system_error = complete ? EIO : EOVERFLOW;
        return false;
    }
    const char* cursor = text;
    const char* end = text + bytes;
    bool any = false;
    while (cursor != end) {
        while (cursor != end && (*cursor == ' ' || *cursor == '\t' ||
                                 *cursor == '\r' || *cursor == '\n' ||
                                 *cursor == ',')) {
            ++cursor;
        }
        if (cursor == end) {
            break;
        }
        std::uint32_t first = 0;
        if (!parse_cpu_number(cursor, end, first)) {
            system_error = EINVAL;
            return false;
        }
        std::uint32_t last = first;
        if (cursor != end && *cursor == '-') {
            ++cursor;
            if (!parse_cpu_number(cursor, end, last) || last < first) {
                system_error = EINVAL;
                return false;
            }
        }
        if (cursor != end && *cursor != ',' && *cursor != ' ' &&
            *cursor != '\t' && *cursor != '\r' && *cursor != '\n') {
            system_error = EINVAL;
            return false;
        }
        if (last >= CPU_SETSIZE) {
            system_error = EOVERFLOW;
            return false;
        }
        for (std::uint32_t cpu = first; cpu <= last; ++cpu) {
            CPU_SET(static_cast<int>(cpu), &output);
            any = true;
            if (cpu == std::numeric_limits<std::uint32_t>::max()) {
                break;
            }
        }
    }
    if (!any) {
        system_error = ENODEV;
    }
    return any;
}

bool read_current_policy(
    rt::WaitStrategy wait_strategy,
    rt::ThreadPolicy& output,
    std::int32_t& system_error) noexcept {
    output = {};
    output.wait_strategy = wait_strategy;
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    int status = pthread_getaffinity_np(
        pthread_self(),
        sizeof(affinity),
        &affinity);
    if (status != 0 || !cpu_set_to_policy(affinity, output.cpu_set)) {
        system_error = status == 0 ? EOVERFLOW : status;
        return false;
    }

    int scheduler = 0;
    sched_param parameters{};
    status = pthread_getschedparam(pthread_self(), &scheduler, &parameters);
    if (status != 0) {
        system_error = status;
        return false;
    }
    output.scheduling_class = scheduling_class(scheduler);
    output.scheduling_priority = parameters.sched_priority;

    char name[rt::thread_name_capacity]{};
    status = pthread_getname_np(pthread_self(), name, sizeof(name));
    if (status != 0) {
        system_error = status;
        return false;
    }
    copy_name(output.name, name);

    pthread_attr_t attributes;
    status = pthread_getattr_np(pthread_self(), &attributes);
    if (status != 0) {
        system_error = status;
        return false;
    }
    std::size_t stack_bytes = 0;
    std::size_t guard_bytes = 0;
    const int stack_status = pthread_attr_getstacksize(&attributes, &stack_bytes);
    const int guard_status = pthread_attr_getguardsize(&attributes, &guard_bytes);
    const int destroy_status = pthread_attr_destroy(&attributes);
    if (stack_status != 0 || guard_status != 0 || destroy_status != 0) {
        system_error = stack_status != 0
            ? stack_status
            : (guard_status != 0 ? guard_status : destroy_status);
        return false;
    }
    output.stack_bytes = stack_bytes;
    output.guard_bytes = guard_bytes;
    output.numa_node = -1;
    system_error = 0;
    return true;
}

bool set_affinity(
    const rt::CpuSetRequest& requested,
    std::int32_t& system_error) noexcept {
    cpu_set_t affinity;
    if (!policy_to_cpu_set(requested, affinity)) {
        system_error = EINVAL;
        return false;
    }
    const int status = pthread_setaffinity_np(
        pthread_self(),
        sizeof(affinity),
        &affinity);
    if (status != 0) {
        system_error = status;
        return false;
    }
    return true;
}

bool set_scheduler(
    rt::SchedulingClass requested,
    std::int32_t priority,
    std::int32_t& system_error) noexcept {
    sched_param parameters{};
    parameters.sched_priority = priority;
    const int status = pthread_setschedparam(
        pthread_self(),
        native_scheduling_class(requested),
        &parameters);
    if (status != 0) {
        system_error = status;
        return false;
    }
    return true;
}

bool set_name(
    const std::array<char, rt::thread_name_capacity>& requested,
    std::int32_t& system_error) noexcept {
    const int status = pthread_setname_np(pthread_self(), requested.data());
    if (status != 0) {
        system_error = status;
        return false;
    }
    return true;
}

bool matches_resolved(
    const rt::ThreadPolicy& resolved,
    const rt::ThreadPolicy& observed) noexcept {
    return resolved.cpu_set.count == observed.cpu_set.count &&
           resolved.cpu_set.cpu_ids == observed.cpu_set.cpu_ids &&
           resolved.scheduling_class == observed.scheduling_class &&
           resolved.scheduling_priority == observed.scheduling_priority &&
           resolved.wait_strategy == observed.wait_strategy &&
           resolved.stack_bytes == observed.stack_bytes &&
           resolved.guard_bytes == observed.guard_bytes &&
           resolved.name == observed.name;
}

#endif

} // namespace

namespace rt::detail {

void ThreadStartupResult::reset() noexcept {
    ready.store(false, std::memory_order_relaxed);
    applied = PolicyOperationState::not_attempted;
    verified = PolicyOperationState::not_attempted;
    read_back = {};
    creation_error = 0;
    apply_error = 0;
    verify_error = 0;
    used_default_fallback = false;
}

void ThreadStartupResult::publish() noexcept {
    ready.store(true, std::memory_order_release);
    ready.notify_all();
}

void ThreadStartupResult::wait() const noexcept {
    while (!ready.load(std::memory_order_acquire)) {
        ready.wait(false, std::memory_order_relaxed);
    }
}

void ThreadStartupGate::reset() noexcept {
    decision_.store(0, std::memory_order_release);
}

void ThreadStartupGate::commit() noexcept {
    decision_.store(1, std::memory_order_release);
    decision_.notify_all();
}

void ThreadStartupGate::abort() noexcept {
    decision_.store(2, std::memory_order_release);
    decision_.notify_all();
}

bool ThreadStartupGate::wait() const noexcept {
    auto decision = decision_.load(std::memory_order_acquire);
    while (decision == 0) {
        decision_.wait(0, std::memory_order_relaxed);
        decision = decision_.load(std::memory_order_acquire);
    }
    return decision == 1;
}

Status ThreadPolicyProvider::before_create(
    ThreadRoleId,
    std::size_t,
    const ThreadRolePlan&,
    std::int32_t& system_error) noexcept {
    system_error = 0;
    return Status::ok;
}

void ThreadPolicyProvider::after_join(
    ThreadRoleId,
    std::size_t) noexcept {}

Status NativeThreadPolicyProvider::resolve(
    ThreadRoleId role,
    PolicyApplicationMode mode,
    bool active,
    bool observable,
    bool has_request,
    const ThreadPolicy& requested,
    const ThreadPolicy& role_default,
    ThreadPolicy& resolved,
    PolicyResolutionState& resolution,
    std::int32_t& system_error) noexcept {
    (void)role;
    resolved = role_default;
    resolution = PolicyResolutionState::portable_default;
    system_error = 0;

    if (!active) {
        resolution = PolicyResolutionState::inactive;
        if (has_request && requested.requirement == PolicyRequirement::strict) {
            system_error = ENODEV;
            return Status::invalid_config;
        }
        return Status::ok;
    }
    if (mode == PolicyApplicationMode::verify_only && !observable) {
        resolution = PolicyResolutionState::external_verify_only;
        if (has_request && requested.requirement == PolicyRequirement::strict) {
            system_error = kUnsupportedError;
            return Status::invalid_config;
        }
        if (has_request) {
            system_error = kUnsupportedError;
        }
        return Status::ok;
    }

#if !defined(__linux__)
    if (has_request &&
        (requested.requirement == PolicyRequirement::strict ||
         nondefault_request(requested))) {
        system_error = kUnsupportedError;
        if (requested.requirement == PolicyRequirement::strict) {
            return Status::invalid_config;
        }
        resolution = PolicyResolutionState::unsupported_best_effort;
        return Status::ok;
    }
    resolution = PolicyResolutionState::portable_default;
    return Status::ok;
#else
    ThreadPolicy baseline;
    if (!read_current_policy(role_default.wait_strategy, baseline, system_error)) {
        if (has_request && requested.requirement == PolicyRequirement::strict) {
            return Status::invalid_config;
        }
        resolution = PolicyResolutionState::unsupported_best_effort;
        resolved = role_default;
        return Status::ok;
    }
    if (mode == PolicyApplicationMode::apply_and_verify) {
        pthread_attr_t defaults;
        int status = pthread_attr_init(&defaults);
        const bool defaults_initialized = status == 0;
        std::size_t default_stack = 0;
        std::size_t default_guard = 0;
        if (status == 0) {
            status = pthread_attr_getstacksize(&defaults, &default_stack);
        }
        if (status == 0) {
            status = pthread_attr_getguardsize(&defaults, &default_guard);
        }
        const int destroy_status = defaults_initialized
            ? pthread_attr_destroy(&defaults)
            : 0;
        if (status == 0) {
            status = destroy_status;
        }
        if (status != 0) {
            system_error = status;
            if (has_request &&
                requested.requirement == PolicyRequirement::strict) {
                return Status::invalid_config;
            }
            resolution = PolicyResolutionState::unsupported_best_effort;
            resolved = role_default;
            return Status::ok;
        }
        baseline.stack_bytes = default_stack;
        baseline.guard_bytes = default_guard;
    }
    resolved = baseline;
    resolved.requirement = requested.requirement;
    resolved.wait_strategy = role_default.wait_strategy;

    const auto fallback = [&](std::int32_t error) {
        system_error = error;
        if (requested.requirement == PolicyRequirement::strict) {
            return Status::invalid_config;
        }
        resolved = baseline;
        resolved.wait_strategy = role_default.wait_strategy;
        resolution = PolicyResolutionState::native_best_effort_fallback;
        return Status::ok;
    };

    cpu_set_t allowed;
    if (!policy_to_cpu_set(baseline.cpu_set, allowed)) {
        return fallback(EOVERFLOW);
    }
    cpu_set_t selected = allowed;
    if (requested.numa_node >= 0) {
        cpu_set_t node;
        std::int32_t node_error = 0;
        if (!read_numa_node_cpus(requested.numa_node, node, node_error)) {
            return fallback(node_error);
        }
        CPU_AND(&selected, &allowed, &node);
    }
    if (requested.cpu_set.count != 0) {
        cpu_set_t explicit_set;
        if (!policy_to_cpu_set(requested.cpu_set, explicit_set)) {
            return fallback(EINVAL);
        }
        for (std::size_t index = 0; index < requested.cpu_set.count; ++index) {
            const auto cpu = requested.cpu_set.cpu_ids[index];
            if (!CPU_ISSET(static_cast<int>(cpu), &selected)) {
                return fallback(ENODEV);
            }
        }
        selected = explicit_set;
    }
    if (requested.cpu_set.count != 0 || requested.numa_node >= 0) {
        if (!cpu_set_to_policy(selected, resolved.cpu_set)) {
            return fallback(ENODEV);
        }
        std::sort(
            resolved.cpu_set.cpu_ids.begin(),
            resolved.cpu_set.cpu_ids.begin() +
                static_cast<std::ptrdiff_t>(resolved.cpu_set.count));
    }
    resolved.numa_node = requested.numa_node;

    if (requested.scheduling_class != SchedulingClass::inherit) {
        const int native = native_scheduling_class(requested.scheduling_class);
        const int minimum = sched_get_priority_min(native);
        const int maximum = sched_get_priority_max(native);
        if (minimum < 0 || maximum < 0 ||
            requested.scheduling_priority < minimum ||
            requested.scheduling_priority > maximum) {
            return fallback(EINVAL);
        }
        resolved.scheduling_class = requested.scheduling_class;
        resolved.scheduling_priority = requested.scheduling_priority;
    }

    if (requested.wait_strategy != WaitStrategy::inherit) {
        if (mode == PolicyApplicationMode::verify_only) {
            return fallback(kUnsupportedError);
        }
        resolved.wait_strategy = requested.wait_strategy;
    }

    if (requested.stack_bytes != 0 || requested.guard_bytes != 0) {
        if (mode == PolicyApplicationMode::verify_only) {
            if (requested.stack_bytes != 0 &&
                requested.stack_bytes != baseline.stack_bytes) {
                return fallback(kUnsupportedError);
            }
            if (requested.guard_bytes != 0 &&
                requested.guard_bytes != baseline.guard_bytes) {
                return fallback(kUnsupportedError);
            }
        } else {
            pthread_attr_t attributes;
            int status = pthread_attr_init(&attributes);
            const bool attributes_initialized = status == 0;
            if (status == 0 && requested.stack_bytes != 0) {
                status = pthread_attr_setstacksize(
                    &attributes,
                    requested.stack_bytes);
            }
            if (status == 0 && requested.guard_bytes != 0) {
                status = pthread_attr_setguardsize(
                    &attributes,
                    requested.guard_bytes);
            }
            std::size_t actual_stack = baseline.stack_bytes;
            std::size_t actual_guard = baseline.guard_bytes;
            if (status == 0) {
                status = pthread_attr_getstacksize(&attributes, &actual_stack);
            }
            if (status == 0) {
                status = pthread_attr_getguardsize(&attributes, &actual_guard);
            }
            const int destroy_status = attributes_initialized
                ? pthread_attr_destroy(&attributes)
                : 0;
            if (status == 0) {
                status = destroy_status;
            }
            if (status != 0 ||
                (requested.stack_bytes != 0 &&
                 actual_stack != requested.stack_bytes) ||
                (requested.guard_bytes != 0 &&
                 actual_guard != requested.guard_bytes)) {
                return fallback(status == 0 ? EINVAL : status);
            }
            resolved.stack_bytes = actual_stack;
            resolved.guard_bytes = actual_guard;
        }
    }

    if (requested.name.front() != '\0') {
        if (policy_name_length(requested.name) > 15) {
            return fallback(ERANGE);
        }
        resolved.name = requested.name;
    }
    resolution = PolicyResolutionState::native_supported;
    return Status::ok;
#endif
}

void NativeThreadPolicyProvider::apply_and_verify_current(
    ThreadRoleId,
    std::size_t,
    const ThreadRolePlan& plan,
    ThreadStartupResult& result) noexcept {
    result.applied = PolicyOperationState::not_attempted;
    result.verified = PolicyOperationState::not_attempted;
    if (plan.resolution == PolicyResolutionState::unsupported_best_effort ||
        plan.resolution == PolicyResolutionState::native_best_effort_fallback) {
        result.applied = PolicyOperationState::unsupported;
        result.verified = PolicyOperationState::unsupported;
        result.used_default_fallback = true;
#if defined(__linux__)
        (void)read_current_policy(
            plan.resolved.wait_strategy,
            result.read_back,
            result.verify_error);
#else
        result.read_back = plan.resolved;
#endif
        return;
    }

#if !defined(__linux__)
    result.applied = PolicyOperationState::succeeded;
    result.verified = PolicyOperationState::succeeded;
    result.read_back = plan.resolved;
#else
    ThreadPolicy original;
    if (!read_current_policy(
            plan.resolved.wait_strategy,
            original,
            result.apply_error)) {
        result.applied = PolicyOperationState::failed;
        result.verified = PolicyOperationState::failed;
        return;
    }
    bool applied = true;
    if (plan.requested.cpu_set.count != 0 ||
        plan.requested.numa_node >= 0) {
        applied = set_affinity(plan.resolved.cpu_set, result.apply_error);
    }
    if (applied &&
        plan.requested.scheduling_class != SchedulingClass::inherit) {
        applied = set_scheduler(
            plan.resolved.scheduling_class,
            plan.resolved.scheduling_priority,
            result.apply_error);
    }
    if (applied && plan.requested.name.front() != '\0') {
        applied = set_name(plan.resolved.name, result.apply_error);
    }
    if (!applied) {
        std::int32_t ignored = 0;
        if (plan.requested.cpu_set.count != 0 ||
            plan.requested.numa_node >= 0) {
            (void)set_affinity(original.cpu_set, ignored);
        }
        if (plan.requested.scheduling_class != SchedulingClass::inherit) {
            (void)set_scheduler(
                original.scheduling_class,
                original.scheduling_priority,
                ignored);
        }
        if (plan.requested.name.front() != '\0') {
            (void)set_name(original.name, ignored);
        }
        result.applied = PolicyOperationState::failed;
        result.verified = PolicyOperationState::failed;
        result.used_default_fallback = true;
        (void)read_current_policy(
            plan.resolved.wait_strategy,
            result.read_back,
            result.verify_error);
        return;
    }
    result.applied = PolicyOperationState::succeeded;
    if (!read_current_policy(
            plan.resolved.wait_strategy,
            result.read_back,
            result.verify_error)) {
        result.verified = PolicyOperationState::failed;
        return;
    }
    result.verified = matches_resolved(plan.resolved, result.read_back)
        ? PolicyOperationState::succeeded
        : PolicyOperationState::mismatched;
#endif
}

void NativeThreadPolicyProvider::verify_current(
    ThreadRoleId,
    const ThreadRolePlan& plan,
    ThreadStartupResult& result) noexcept {
    result.applied = PolicyOperationState::not_attempted;
    if (plan.resolution == PolicyResolutionState::external_verify_only ||
        plan.resolution == PolicyResolutionState::unsupported_best_effort ||
        plan.resolution == PolicyResolutionState::native_best_effort_fallback) {
        result.verified = PolicyOperationState::unsupported;
        result.used_default_fallback =
            plan.resolution == PolicyResolutionState::native_best_effort_fallback;
        return;
    }
#if !defined(__linux__)
    result.read_back = plan.resolved;
    result.verified = PolicyOperationState::succeeded;
#else
    if (!read_current_policy(
            plan.resolved.wait_strategy,
            result.read_back,
            result.verify_error)) {
        result.verified = PolicyOperationState::failed;
        return;
    }
    result.verified = matches_resolved(plan.resolved, result.read_back)
        ? PolicyOperationState::succeeded
        : PolicyOperationState::mismatched;
#endif
}

NativeThread::~NativeThread() {
    join();
}

Status NativeThread::start(
    ThreadPolicyProvider& provider,
    ThreadStartupGate& gate,
    const ThreadRolePlan& plan,
    std::size_t instance_index,
    ThreadStartupResult& result,
    NativeThreadEntry entry,
    void* entry_data) noexcept {
    if (joinable() || entry == nullptr) {
        return Status::invalid_state;
    }
    result.reset();
    body_started_.store(false, std::memory_order_relaxed);
    provider_ = &provider;
    gate_ = &gate;
    plan_ = &plan;
    result_ = &result;
    instance_index_ = instance_index;
    role_ = plan.role;
    entry_ = entry;
    entry_data_ = entry_data;

    std::int32_t error = 0;
    const auto before = provider.before_create(
        plan.role,
        instance_index,
        plan,
        error);
    if (before != Status::ok) {
        result.creation_error = error;
        result.applied = PolicyOperationState::failed;
        result.publish();
        return before;
    }

#if defined(__linux__)
    pthread_attr_t attributes;
    int status = pthread_attr_init(&attributes);
    const bool attributes_initialized = status == 0;
    if (status == 0 && plan.resolved.stack_bytes != 0) {
        status = pthread_attr_setstacksize(
            &attributes,
            plan.resolved.stack_bytes);
    }
    if (status == 0) {
        status = pthread_attr_setguardsize(
            &attributes,
            plan.resolved.guard_bytes);
    }
    int create_status = status;
    if (create_status == 0) {
        create_status = pthread_create(
            &thread_,
            &attributes,
            &NativeThread::pthread_entry,
            this);
    }
    const int destroy_status = attributes_initialized
        ? pthread_attr_destroy(&attributes)
        : 0;
    if (create_status != 0) {
        result.creation_error = create_status;
        result.applied = PolicyOperationState::failed;
        result.publish();
        return create_status == EAGAIN || create_status == ENOMEM
            ? Status::resource_exhausted
            : Status::internal_error;
    }
    joinable_ = true;
    if (destroy_status != 0) {
        // A successfully created thread remains owned and joinable. Attribute
        // destruction cannot invalidate it, so retain ownership and let the
        // native policy/readback result determine startup success.
        result.creation_error = destroy_status;
    }
#else
    try {
        thread_ = std::thread([this] { run_entry(*this); });
    } catch (const std::bad_alloc&) {
        result.creation_error = ENOMEM;
        result.applied = PolicyOperationState::failed;
        result.publish();
        return Status::resource_exhausted;
    } catch (...) {
        result.creation_error = EAGAIN;
        result.applied = PolicyOperationState::failed;
        result.publish();
        return Status::resource_exhausted;
    }
#endif
    result.wait();
    return Status::ok;
}

void NativeThread::run_entry(NativeThread& self) noexcept {
    self.provider_->apply_and_verify_current(
        self.plan_->role,
        self.instance_index_,
        *self.plan_,
        *self.result_);
    self.result_->publish();
    if (!self.gate_->wait()) {
        return;
    }
    self.body_started_.store(true, std::memory_order_release);
    self.body_started_.notify_all();
    self.entry_(self.entry_data_);
}

#if defined(__linux__)
void* NativeThread::pthread_entry(void* self) noexcept {
    run_entry(*static_cast<NativeThread*>(self));
    return nullptr;
}
#endif

void NativeThread::join() noexcept {
#if defined(__linux__)
    if (joinable_) {
        (void)pthread_join(thread_, nullptr);
        joinable_ = false;
        provider_->after_join(role_, instance_index_);
    }
#else
    if (thread_.joinable()) {
        thread_.join();
        provider_->after_join(role_, instance_index_);
    }
#endif
}

bool NativeThread::joinable() const noexcept {
#if defined(__linux__)
    return joinable_;
#else
    return thread_.joinable();
#endif
}

void NativeThread::wait_started() const noexcept {
    while (!body_started_.load(std::memory_order_acquire)) {
        body_started_.wait(false, std::memory_order_relaxed);
    }
}

ThreadRolePlan make_thread_role_plan(
    const ThreadPolicyReport& report) noexcept {
    return {
        report.role,
        report.requested,
        report.resolved,
        report.resolution,
    };
}

void reset_thread_report_operations(ThreadPolicyReport& report) noexcept {
    report.applied = PolicyOperationState::not_attempted;
    report.verified = PolicyOperationState::not_attempted;
    report.applied_policy = {};
    report.read_back = {};
    report.attempted_instance_count = 0;
    report.applied_instance_count = 0;
    report.verified_instance_count = 0;
    report.fallback_instance_count = 0;
    report.apply_error = 0;
    report.verify_error = 0;
}

void aggregate_thread_startup_results(
    ThreadPolicyReport& report,
    const ThreadStartupResult* results,
    std::size_t count) noexcept {
    reset_thread_report_operations(report);
    if (!results || count == 0) {
        return;
    }
    bool any_apply_failed = false;
    bool any_apply_unsupported = false;
    bool any_verify_failed = false;
    bool any_verify_unsupported = false;
    bool any_verify_mismatched = false;
    for (std::size_t index = 0; index < count; ++index) {
        const auto& result = results[index];
        if (!result.ready.load(std::memory_order_acquire)) {
            continue;
        }
        if (report.attempted_instance_count == 0) {
            report.read_back = result.read_back;
        }
        ++report.attempted_instance_count;
        if (result.applied == PolicyOperationState::succeeded) {
            ++report.applied_instance_count;
        } else if (result.applied == PolicyOperationState::failed) {
            any_apply_failed = true;
        } else if (result.applied == PolicyOperationState::unsupported) {
            any_apply_unsupported = true;
        }
        if (result.verified == PolicyOperationState::succeeded) {
            ++report.verified_instance_count;
        } else if (result.verified == PolicyOperationState::failed) {
            any_verify_failed = true;
        } else if (result.verified == PolicyOperationState::unsupported) {
            any_verify_unsupported = true;
        } else if (result.verified == PolicyOperationState::mismatched) {
            any_verify_mismatched = true;
        }
        if (result.applied != PolicyOperationState::succeeded ||
            result.verified != PolicyOperationState::succeeded) {
            // Preserve an offending instance's actual observation rather than
            // leaving a successful first instance as the aggregate example.
            report.read_back = result.read_back;
        }
        report.fallback_instance_count += result.used_default_fallback ? 1u : 0u;
        if (report.apply_error == 0) {
            report.apply_error = result.creation_error != 0
                ? result.creation_error
                : result.apply_error;
        }
        if (report.verify_error == 0) {
            report.verify_error = result.verify_error;
        }
    }
    if (any_apply_failed) {
        report.applied = PolicyOperationState::failed;
    } else if (any_apply_unsupported) {
        report.applied = PolicyOperationState::unsupported;
    } else if (report.applied_instance_count == count) {
        report.applied = PolicyOperationState::succeeded;
        report.applied_policy = report.resolved;
    }
    if (any_verify_mismatched) {
        report.verified = PolicyOperationState::mismatched;
    } else if (any_verify_failed) {
        report.verified = PolicyOperationState::failed;
    } else if (any_verify_unsupported) {
        report.verified = PolicyOperationState::unsupported;
    } else if (report.verified_instance_count == count) {
        report.verified = PolicyOperationState::succeeded;
    }
}

} // namespace rt::detail
