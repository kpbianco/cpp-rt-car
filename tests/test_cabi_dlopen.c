#include "api/cabi.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

typedef rtfw_handle *(*rtfw_create_fn)(double);
typedef rtfw_status (*rtfw_step_fn)(rtfw_handle *);
typedef void (*rtfw_destroy_fn)(rtfw_handle *);

static int check_symbol(void *sym, const char *name) {
    if (!sym) {
        const char *err = dlerror();
        fprintf(stderr, "missing symbol %s: %s\n", name, err ? err : "unknown error");
        return 0;
    }
    return 1;
}

int main(void) {
#ifndef RTFW_LIB_PATH
#error "RTFW_LIB_PATH must be defined with the runtime library path"
#endif

    void *handle = dlopen(RTFW_LIB_PATH, RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return EXIT_FAILURE;
    }

    dlerror();
    rtfw_create_fn create_fn = (rtfw_create_fn)dlsym(handle, "rtfw_create");
    if (!check_symbol((void *)create_fn, "rtfw_create")) {
        dlclose(handle);
        return EXIT_FAILURE;
    }

    dlerror();
    rtfw_step_fn step_fn = (rtfw_step_fn)dlsym(handle, "rtfw_step");
    if (!check_symbol((void *)step_fn, "rtfw_step")) {
        dlclose(handle);
        return EXIT_FAILURE;
    }

    dlerror();
    rtfw_destroy_fn destroy_fn = (rtfw_destroy_fn)dlsym(handle, "rtfw_destroy");
    if (!check_symbol((void *)destroy_fn, "rtfw_destroy")) {
        dlclose(handle);
        return EXIT_FAILURE;
    }

    if (step_fn(NULL) != RTFW_STATUS_INVALID_ARGUMENT) {
        fprintf(stderr, "rtfw_step should reject NULL handles\n");
        dlclose(handle);
        return EXIT_FAILURE;
    }

    rtfw_handle *instance = create_fn(2.0);
    if (!instance) {
        fprintf(stderr, "rtfw_create returned NULL\n");
        dlclose(handle);
        return EXIT_FAILURE;
    }

    for (int i = 0; i < 5; ++i) {
        rtfw_status st = step_fn(instance);
        if (st != RTFW_STATUS_OK) {
            fprintf(stderr, "step %d failed with status %d\n", i, st);
            destroy_fn(instance);
            dlclose(handle);
            return EXIT_FAILURE;
        }
    }

    destroy_fn(instance);
    destroy_fn(NULL);

    if (dlclose(handle) != 0) {
        fprintf(stderr, "dlclose failed: %s\n", dlerror());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
