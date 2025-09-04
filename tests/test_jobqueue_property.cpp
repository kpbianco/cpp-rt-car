// Property-based FIFO test for the job queue. Built only when RapidCheck is
// available. If RapidCheck headers are missing, compile a dummy test that is
// skipped at runtime so regular builds remain warning-free.
#if __has_include(<rapidcheck/gtest.h>)
#  include <rapidcheck/gtest.h>
#  include "simcore/job_queue.hpp"
#  include <vector>

RC_GTEST_PROP(JobQueueProperty, PushPopPreservesOrder,
              (const std::vector<int>& data)) {
    BoundedMPMCQueue<int> q(data.size() + 1);
    for (int v : data) {
        RC_ASSERT(q.try_push(v));
    }
    for (int v : data) {
        int out = 0;
        RC_ASSERT(q.try_pop(out));
        RC_ASSERT(out == v);
    }
    int dummy = 0;
    RC_ASSERT(!q.try_pop(dummy));
}
#else
#  include <gtest/gtest.h>

TEST(JobQueueProperty, RapidCheckNotAvailable) {
    GTEST_SKIP() << "RapidCheck not available";
}
#endif
