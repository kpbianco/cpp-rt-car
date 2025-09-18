#include "api/cabi.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
typedef HMODULE rtfw_lib_handle;

static void lib_clear_error(void) {
    SetLastError(0);
}

static const char *lib_error(void) {
    static char buffer[256];
    DWORD err = GetLastError();
    if (err == 0) {
        return "unknown error";
    }
    DWORD len = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               NULL, err, 0, buffer, (DWORD)sizeof(buffer), NULL);
    if (len == 0) {
        snprintf(buffer, sizeof(buffer), "error code %lu", (unsigned long)err);
    } else {
        while (len > 0 && (buffer[len - 1] == '\r' || buffer[len - 1] == '\n')) {
            buffer[--len] = '\0';
        }
    }
    return buffer;
}

static rtfw_lib_handle lib_open(const char *path) {
    lib_clear_error();
    return LoadLibraryA(path);
}

static void *lib_symbol(rtfw_lib_handle handle, const char *name) {
    lib_clear_error();
    return (void *)GetProcAddress(handle, name);
}

static int lib_close(rtfw_lib_handle handle) {
    if (FreeLibrary(handle)) {
        return 0;
    }
    return -1;
}
#else
#    include <dlfcn.h>
typedef void *rtfw_lib_handle;

static void lib_clear_error(void) {
    dlerror();
}

static const char *lib_error(void) {
    const char *err = dlerror();
    return err ? err : "unknown error";
}

static rtfw_lib_handle lib_open(const char *path) {
    return dlopen(path, RTLD_NOW);
}

static void *lib_symbol(rtfw_lib_handle handle, const char *name) {
    return dlsym(handle, name);
}

static int lib_close(rtfw_lib_handle handle) {
    return dlclose(handle);
}
#endif

typedef rtfw_handle *(*rtfw_create_fn)(double);
typedef rtfw_status (*rtfw_step_fn)(rtfw_handle *);
typedef void (*rtfw_destroy_fn)(rtfw_handle *);

static int check_symbol(void *sym, const char *name) {
    if (!sym) {
        fprintf(stderr, "missing symbol %s: %s\n", name, lib_error());
        return 0;
    }
    return 1;
}

int main(void) {
#ifndef RTFW_LIB_PATH
#error "RTFW_LIB_PATH must be defined with the runtime library path"
#endif

    rtfw_lib_handle handle = lib_open(RTFW_LIB_PATH);
    if (!handle) {
        fprintf(stderr, "failed to load runtime: %s\n", lib_error());
        return EXIT_FAILURE;
    }

    lib_clear_error();
    rtfw_create_fn create_fn = (rtfw_create_fn)lib_symbol(handle, "rtfw_create");
    if (!check_symbol((void *)create_fn, "rtfw_create")) {
        lib_close(handle);
        return EXIT_FAILURE;
    }

    lib_clear_error();
    rtfw_step_fn step_fn = (rtfw_step_fn)lib_symbol(handle, "rtfw_step");
    if (!check_symbol((void *)step_fn, "rtfw_step")) {
        lib_close(handle);
        return EXIT_FAILURE;
    }

    lib_clear_error();
    rtfw_destroy_fn destroy_fn = (rtfw_destroy_fn)lib_symbol(handle, "rtfw_destroy");
    if (!check_symbol((void *)destroy_fn, "rtfw_destroy")) {
        lib_close(handle);
        return EXIT_FAILURE;
    }

    if (step_fn(NULL) != RTFW_STATUS_INVALID_ARGUMENT) {
        fprintf(stderr, "rtfw_step should reject NULL handles\n");
        lib_close(handle);
        return EXIT_FAILURE;
    }

    rtfw_handle *instance = create_fn(2.0);
    if (!instance) {
        fprintf(stderr, "rtfw_create returned NULL\n");
        lib_close(handle);
        return EXIT_FAILURE;
    }

    for (int i = 0; i < 5; ++i) {
        rtfw_status st = step_fn(instance);
        if (st != RTFW_STATUS_OK) {
            fprintf(stderr, "step %d failed with status %d\n", i, st);
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
    }

    destroy_fn(instance);
    destroy_fn(NULL);

    if (lib_close(handle) != 0) {
        fprintf(stderr, "failed to unload runtime: %s\n", lib_error());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
