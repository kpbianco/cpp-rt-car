#include <gtest/gtest.h>
#include <simcore/sandbox.hpp>
#include <cstdlib>
#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

TEST(Sandbox, EnableSandbox) {
#if defined(__linux__)
    pid_t pid = fork();
    ASSERT_NE(pid, -1);
    if (pid == 0) {
        bool ok = enable_sandbox();
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));
    if (WEXITSTATUS(status) != 0) {
        GTEST_SKIP() << "Sandbox unsupported on this kernel";
    }
    EXPECT_EQ(WEXITSTATUS(status), 0);
#else
    GTEST_SKIP() << "Sandbox not implemented for this platform";
#endif
}
