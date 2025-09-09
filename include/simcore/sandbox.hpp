#pragma once

// Simple cross-platform sandbox helper.
// On Linux this installs a small seccomp filter to restrict system calls.
// On Windows it creates a Job Object to ensure the process is killed
// if the parent terminates.

#if defined(__linux__)
#include <sys/prctl.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstddef>
inline bool enable_sandbox()
{
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return false;

    struct sock_filter filter[] = {
        // load the system call number into accumulator
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        // allow read
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_read, 0, 6),
        // allow write
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_write, 0, 5),
        // allow exit
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_exit, 0, 4),
        // allow exit_group
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_exit_group, 0, 3),
        // allow sigreturn
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_rt_sigreturn, 0, 2),
        // kill process for anything else
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
        // allow the call
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = {
        .len = static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0)
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

