#include <stdio.h>
#include <stdlib.h>

#include "api/cabi.h"

static void log_features(void) {
    cabi_version_t version = cabi_version();
    cabi_feature_flags features = cabi_get_features();
    printf("mini_app: cabi version %u.%u, features=0x%08x\n", version.major, version.minor, features);
}

static int run_frames(rtfw_handle *handle, int max_frames) {
    for (int frame = 0; frame < max_frames; ++frame) {
        const rtfw_status status = rtfw_step(handle);
        if (status == RTFW_STATUS_OK) {
            printf("mini_app: frame %d ok\n", frame);
            continue;
        }
        if (status == RTFW_STATUS_COMPLETE) {
            printf("mini_app: frame %d complete\n", frame);
            return EXIT_SUCCESS;
        }
        fprintf(stderr, "mini_app: frame %d error (status=%d)\n", frame, (int)status);
        return EXIT_FAILURE;
    }

    printf("mini_app: reached max frames without completion\n");
    return EXIT_SUCCESS;
}

int main(void) {
    log_features();

    rtfw_handle *handle = rtfw_create(5.0);
    if (!handle) {
        fprintf(stderr, "mini_app: failed to create runtime\n");
        return EXIT_FAILURE;
    }

    int result = run_frames(handle, 5);

    rtfw_destroy(handle);
    printf("mini_app: shutdown\n");

    return result;
}
