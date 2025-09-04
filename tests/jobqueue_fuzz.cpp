#include <cstddef>
#include <cstdint>
#include "simcore/job_queue.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    BoundedMPMCQueue<uint8_t> q(size + 1);
    for (size_t i = 0; i < size; ++i) {
        q.try_push(data[i]);
    }
    uint8_t out = 0;
    while (q.try_pop(out)) {
        // consume
    }
    return 0;
}
