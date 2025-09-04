#include <rapidcheck/gtest.h>
#include "simcore/job_queue.hpp"
#include <vector>

RC_GTEST_PROP(JobQueueProperty, PushPopPreservesOrder, (const std::vector<int>& data)) {
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
