#include "native_platform_preflight.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace {

void set_check(
    rt::PlatformPreflightReport& report,
    rt::PlatformCheckId id,
    rt::PlatformCheckStatus status,
    std::int32_t system_error,
    const char* message) noexcept {
    if (report.check_count >= report.checks.size()) {
        report.passed = false;
        return;
    }
    auto& check = report.checks[report.check_count++];
    check.id = id;
    check.status = status;
    check.system_error = system_error;
    std::snprintf(
        check.message.data(),
        check.message.size(),
        "%s",
        message ? message : "");
    if (status != rt::PlatformCheckStatus::passed) {
        report.passed = false;
    }
}

#if defined(__linux__)

bool read_text(
    const char* path,
    char* buffer,
    std::size_t capacity,
    std::int32_t& system_error) noexcept {
    system_error = 0;
    if (!path || !buffer || capacity < 2) {
        system_error = EINVAL;
        return false;
    }
    const int descriptor = ::open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        system_error = errno;
        buffer[0] = '\0';
        return false;
    }
    const auto count = ::read(descriptor, buffer, capacity - 1);
    const auto read_error = errno;
    (void)::close(descriptor);
    if (count < 0) {
        system_error = read_error;
        buffer[0] = '\0';
        return false;
    }
    buffer[static_cast<std::size_t>(count)] = '\0';
    return true;
}

bool kernel_is_realtime(std::int32_t& system_error) noexcept {
    char realtime[32]{};
    if (read_text(
            "/sys/kernel/realtime",
            realtime,
            sizeof(realtime),
            system_error)) {
        return realtime[0] == '1';
    }

    struct utsname identity {};
    if (::uname(&identity) != 0) {
        system_error = errno;
        return false;
    }
    system_error = 0;
    return std::strstr(identity.release, "PREEMPT_RT") != nullptr ||
        std::strstr(identity.version, "PREEMPT_RT") != nullptr;
}

bool parse_cpu_list(
    const char* text,
    cpu_set_t& mask) noexcept {
    CPU_ZERO(&mask);
    bool found = false;
    const char* cursor = text;
    while (cursor && *cursor) {
        while (*cursor == ',' || *cursor == ' ' ||
               *cursor == '\t' || *cursor == '\n') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        char* end = nullptr;
        errno = 0;
        const long first = std::strtol(cursor, &end, 10);
        if (end == cursor || errno != 0 || first < 0) {
            return false;
        }
        cursor = end;
        long last = first;
        if (*cursor == '-') {
            ++cursor;
            errno = 0;
            last = std::strtol(cursor, &end, 10);
            if (end == cursor || errno != 0 || last < first) {
                return false;
            }
            cursor = end;
        }
        if (last >= CPU_SETSIZE) {
            return false;
        }
        for (long cpu = first; cpu <= last; ++cpu) {
            CPU_SET(static_cast<int>(cpu), &mask);
            found = true;
        }
        if (*cursor != '\0' && *cursor != ',' &&
            *cursor != ' ' && *cursor != '\t' &&
            *cursor != '\n') {
            return false;
        }
    }
    return found;
}

bool affinity_is_isolated(std::int32_t& system_error) noexcept {
    char isolated_text[4096]{};
    if (!read_text(
            "/sys/devices/system/cpu/isolated",
            isolated_text,
            sizeof(isolated_text),
            system_error)) {
        return false;
    }
    cpu_set_t isolated;
    if (!parse_cpu_list(isolated_text, isolated)) {
        system_error = ENODATA;
        return false;
    }

    cpu_set_t current;
    CPU_ZERO(&current);
    if (::sched_getaffinity(0, sizeof(current), &current) != 0) {
        system_error = errno;
        return false;
    }
    bool any = false;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &current)) {
            any = true;
            if (!CPU_ISSET(cpu, &isolated)) {
                system_error = EXDEV;
                return false;
            }
        }
    }
    system_error = any ? 0 : ENODATA;
    return any;
}

bool memory_lock_limit_covers(
    std::size_t planned_runtime_bytes,
    std::int32_t& system_error) noexcept {
    struct rlimit limit {};
    if (::getrlimit(RLIMIT_MEMLOCK, &limit) != 0) {
        system_error = errno;
        return false;
    }
    system_error = 0;
    if (limit.rlim_cur == RLIM_INFINITY) {
        return true;
    }
    return limit.rlim_cur >=
        static_cast<rlim_t>(planned_runtime_bytes);
}

bool locked_memory_covers(
    std::size_t planned_runtime_bytes,
    std::int32_t& system_error) noexcept {
    char status[16 * 1024]{};
    if (!read_text(
            "/proc/self/status",
            status,
            sizeof(status),
            system_error)) {
        return false;
    }
    const char* field = std::strstr(status, "VmLck:");
    if (!field) {
        system_error = ENODATA;
        return false;
    }
    field += std::strlen("VmLck:");
    while (*field == ' ' || *field == '\t') {
        ++field;
    }
    char* end = nullptr;
    errno = 0;
    const auto kibibytes = std::strtoull(field, &end, 10);
    if (end == field || errno != 0) {
        system_error = errno ? errno : EINVAL;
        return false;
    }
    if (kibibytes >
        std::numeric_limits<std::uint64_t>::max() / 1024u) {
        system_error = EOVERFLOW;
        return false;
    }
    system_error = 0;
    return kibibytes * 1024u >= planned_runtime_bytes;
}

bool realtime_scheduler_active(
    std::int32_t& system_error) noexcept {
    const int policy = ::sched_getscheduler(0);
    if (policy < 0) {
        system_error = errno;
        return false;
    }
    struct sched_param parameters {};
    if (::sched_getparam(0, &parameters) != 0) {
        system_error = errno;
        return false;
    }
    system_error = 0;
    return (policy == SCHED_FIFO || policy == SCHED_RR) &&
        parameters.sched_priority > 0;
}

#endif

} // namespace

namespace rt::detail {

void NativePlatformPreflightProbe::inspect(
    std::size_t planned_runtime_bytes,
    const RuntimeClock& clock,
    PlatformPreflightReport& report) noexcept {
    report = {};
    report.mode = PlatformPreflightMode::strict;
    report.passed = true;

    const bool absolute_monotonic_clock =
        clock.supports_absolute_sleep() &&
        std::chrono::steady_clock::is_steady;
    set_check(
        report,
        PlatformCheckId::absolute_monotonic_clock,
        absolute_monotonic_clock
            ? PlatformCheckStatus::passed
            : PlatformCheckStatus::failed,
        0,
        absolute_monotonic_clock
            ? "absolute monotonic wait is available"
            : "absolute monotonic wait is unavailable");

#if defined(__linux__)
    std::int32_t error = 0;
    const bool realtime_kernel = kernel_is_realtime(error);
    set_check(
        report,
        PlatformCheckId::realtime_kernel,
        realtime_kernel
            ? PlatformCheckStatus::passed
            : PlatformCheckStatus::failed,
        error,
        realtime_kernel
            ? "PREEMPT_RT kernel detected"
            : "PREEMPT_RT kernel was not detected");

    error = 0;
    const bool lock_limit =
        memory_lock_limit_covers(planned_runtime_bytes, error);
    set_check(
        report,
        PlatformCheckId::memory_lock_limit,
        lock_limit
            ? PlatformCheckStatus::passed
            : PlatformCheckStatus::failed,
        error,
        lock_limit
            ? "RLIMIT_MEMLOCK covers the runtime memory plan"
            : "RLIMIT_MEMLOCK is below the runtime memory plan");

    error = 0;
    const bool locked =
        locked_memory_covers(planned_runtime_bytes, error);
    set_check(
        report,
        PlatformCheckId::locked_memory,
        locked
            ? PlatformCheckStatus::passed
            : PlatformCheckStatus::failed,
        error,
        locked
            ? "locked process memory covers the runtime memory plan"
            : "locked process memory is below the runtime memory plan");

    error = 0;
    const bool isolated = affinity_is_isolated(error);
    set_check(
        report,
        PlatformCheckId::isolated_cpu_affinity,
        isolated
            ? PlatformCheckStatus::passed
            : PlatformCheckStatus::failed,
        error,
        isolated
            ? "current affinity is restricted to isolated CPUs"
            : "current affinity is not restricted to isolated CPUs");

    error = 0;
    const bool realtime_policy =
        realtime_scheduler_active(error);
    set_check(
        report,
        PlatformCheckId::realtime_scheduler,
        realtime_policy
            ? PlatformCheckStatus::passed
            : PlatformCheckStatus::failed,
        error,
        realtime_policy
            ? "current thread uses SCHED_FIFO or SCHED_RR"
            : "current thread lacks realtime scheduling policy");
#else
    for (const auto id : {
             PlatformCheckId::realtime_kernel,
             PlatformCheckId::memory_lock_limit,
             PlatformCheckId::locked_memory,
             PlatformCheckId::isolated_cpu_affinity,
             PlatformCheckId::realtime_scheduler}) {
        set_check(
            report,
            id,
            PlatformCheckStatus::unsupported,
            0,
            "strict realtime prerequisite is unsupported on this platform");
    }
#endif
}

} // namespace rt::detail
