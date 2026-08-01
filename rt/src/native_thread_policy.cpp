#include "native_thread_policy.hpp"

#include <algorithm>
#include <cerrno>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace {

#if defined(__linux__)
rt::Status native_status(int error) noexcept {
    if (error == EINVAL) {
        return rt::Status::invalid_config;
    }
    if (error == EAGAIN || error == ENOMEM) {
        return rt::Status::resource_exhausted;
    }
    return rt::Status::internal_error;
}

int scheduling_policy(rt::SchedulingClass scheduling_class) noexcept {
    switch (scheduling_class) {
    case rt::SchedulingClass::normal:
        return SCHED_OTHER;
    case rt::SchedulingClass::fifo:
        return SCHED_FIFO;
    case rt::SchedulingClass::round_robin:
        return SCHED_RR;
    case rt::SchedulingClass::inherit:
        break;
    }
    return -1;
}

rt::SchedulingClass scheduling_class(int policy) noexcept {
    switch (policy) {
    case SCHED_OTHER:
        return rt::SchedulingClass::normal;
    case SCHED_FIFO:
        return rt::SchedulingClass::fifo;
    case SCHED_RR:
        return rt::SchedulingClass::round_robin;
    default:
        return rt::SchedulingClass::inherit;
    }
}
#endif

} // namespace

namespace rt::detail {

ThreadPolicyProviderCapabilities
NativeThreadPolicyProvider::capabilities() const noexcept {
#if defined(__linux__)
    return ThreadPolicyProviderCapabilities{
        true,
        true,
        true,
        16,
    };
#else
    return {};
#endif
}

Status NativeThreadPolicyProvider::apply_current_thread(
    ThreadResourceId id,
    const ThreadPolicy& policy,
    ThreadPolicy& applied,
    int& system_error) noexcept {
    (void)id;
    applied = {};
    applied.requirement = policy.requirement;
    system_error = 0;
#if defined(__linux__)
    if (policy.cpu_set.specified) {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (std::size_t cpu = 0;
             cpu < policy.cpu_set.logical_cpu_count;
             ++cpu) {
            const auto word = cpu / 64;
            const auto bit = cpu % 64;
            if ((policy.cpu_set.words[word] &
                    (std::uint64_t{1} << bit)) != 0) {
                CPU_SET(static_cast<int>(cpu), &set);
            }
        }
        const int result = ::pthread_setaffinity_np(
            ::pthread_self(),
            sizeof(set),
            &set);
        if (result != 0) {
            system_error = result;
            return native_status(result);
        }
        applied.cpu_set = policy.cpu_set;
    }

    if (policy.scheduling_class != SchedulingClass::inherit) {
        sched_param parameters{};
        parameters.sched_priority =
            static_cast<int>(policy.scheduling_priority);
        const int result = ::pthread_setschedparam(
            ::pthread_self(),
            scheduling_policy(policy.scheduling_class),
            &parameters);
        if (result != 0) {
            system_error = result;
            return native_status(result);
        }
        applied.scheduling_class = policy.scheduling_class;
        applied.scheduling_priority = policy.scheduling_priority;
    }

    if (policy.name.front() != '\0') {
        const int result = ::pthread_setname_np(
            ::pthread_self(),
            policy.name.data());
        if (result != 0) {
            system_error = result;
            return native_status(result);
        }
        applied.name = policy.name;
    }
    return Status::ok;
#else
    (void)policy;
    return Status::invalid_config;
#endif
}

Status NativeThreadPolicyProvider::inspect_current_thread(
    ThreadResourceId id,
    ThreadPolicy& observed,
    int& system_error) noexcept {
    (void)id;
    observed = {};
    system_error = 0;
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    int result = ::pthread_getaffinity_np(
        ::pthread_self(),
        sizeof(set),
        &set);
    if (result != 0) {
        system_error = result;
        return native_status(result);
    }
    observed.cpu_set.specified = true;
    observed.cpu_set.logical_cpu_count =
        static_cast<std::uint16_t>(policy_cpu_capacity);
    for (std::size_t cpu = 0; cpu < policy_cpu_capacity; ++cpu) {
        if (CPU_ISSET(static_cast<int>(cpu), &set)) {
            observed.cpu_set.words[cpu / 64] |=
                std::uint64_t{1} << (cpu % 64);
        }
    }

    int policy = 0;
    sched_param parameters{};
    result = ::pthread_getschedparam(
        ::pthread_self(),
        &policy,
        &parameters);
    if (result != 0) {
        system_error = result;
        return native_status(result);
    }
    observed.scheduling_class = scheduling_class(policy);
    observed.scheduling_priority = static_cast<std::uint8_t>(
        std::clamp(parameters.sched_priority, 0, 255));

    std::array<char, policy_thread_name_capacity> name{};
    result = ::pthread_getname_np(
        ::pthread_self(),
        name.data(),
        name.size());
    if (result != 0) {
        system_error = result;
        return native_status(result);
    }
    observed.name = name;
    return Status::ok;
#else
    return Status::invalid_config;
#endif
}

} // namespace rt::detail
