#pragma once

// Simple cross-platform sandbox helper.
// On Linux this enables seccomp strict mode to restrict system calls.
// On Windows it creates a Job Object to ensure the process is killed
// if the parent terminates.

#if defined(__linux__)
#include <sys/prctl.h>
#include <linux/seccomp.h>
#include <unistd.h>
inline bool enable_sandbox()
{
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return false;
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_STRICT) != 0)
        return false;
    return true;
}
#elif defined(_WIN32)
#define NOMINMAX
#include <windows.h>
inline bool enable_sandbox()
{
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &info, sizeof(info))) {
        CloseHandle(job);
        return false;
    }
    if (!AssignProcessToJobObject(job, GetCurrentProcess())) {
        CloseHandle(job);
        return false;
    }
    return true;
}
#else
inline bool enable_sandbox() { return false; }
#endif

