#include <rt/c_api.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

typedef void (*rtfw_config_init_fn)(rtfw_config *);
typedef rtfw_status (*rtfw_config_set_fn)(rtfw_config *, const char *, const char *);
typedef rt_capabilities_c (*rt_query_capabilities_fn)(void);
typedef const char *(*rtfw_status_message_fn)(rtfw_status);
typedef void (*rtfw_frame_context_init_fn)(rtfw_frame_context *);
typedef void (*rtfw_step_result_init_fn)(rtfw_step_result *);
typedef rtfw_status (*rtfw_create_fn)(const rtfw_config *, rtfw_handle **);
typedef rtfw_status (*rtfw_register_callback_fn)(
    rtfw_handle *, const char *, rtfw_frame_callback, void *);
typedef rtfw_status (*rtfw_lifecycle_fn)(rtfw_handle *);
typedef rtfw_status (*rtfw_step_fn)(
    rtfw_handle *, const rtfw_frame_context *, rtfw_step_result *);
typedef rtfw_status (*rtfw_get_state_fn)(
    const rtfw_handle *, rtfw_runtime_state *);
typedef const char *(*rtfw_last_error_fn)(const rtfw_handle *);
typedef void (*rtfw_destroy_fn)(rtfw_handle *);

static int check_symbol(void *sym, const char *name) {
    if (!sym) {
        fprintf(stderr, "missing symbol %s: %s\n", name, lib_error());
        return 0;
    }
    return 1;
}

#define LOAD_FUNCTION(target, symbol_name)                                      \
    do {                                                                        \
        void *raw_symbol = lib_symbol(handle, symbol_name);                     \
        if (!check_symbol(raw_symbol, symbol_name)) {                           \
            lib_close(handle);                                                  \
            return EXIT_FAILURE;                                                \
        }                                                                       \
        if (sizeof(target) != sizeof(raw_symbol)) {                             \
            fprintf(stderr, "incompatible loader pointer size for %s\n",        \
                    symbol_name);                                                \
            lib_close(handle);                                                  \
            return EXIT_FAILURE;                                                \
        }                                                                       \
        memcpy(&(target), &raw_symbol, sizeof(target));                         \
    } while (0)

typedef struct callback_state {
    uint64_t calls;
    uint64_t last_frame;
} callback_state;

static rtfw_callback_result test_callback(
    void *user_data,
    const rtfw_callback_context *context) {
    callback_state *state = (callback_state *)user_data;
    if (!state || !context ||
        context->struct_size < sizeof(rtfw_callback_context) ||
        context->delta_ns != 1000000 ||
        context->scratch_bytes != 64) {
        return RTFW_CALLBACK_ERROR;
    }
    ++state->calls;
    state->last_frame = context->frame_index;
    return RTFW_CALLBACK_OK;
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
    rtfw_config_init_fn config_init_fn;
    rtfw_config_set_fn config_set_fn;
    rt_query_capabilities_fn capabilities_fn;
    rtfw_status_message_fn status_message_fn;
    rtfw_frame_context_init_fn frame_init_fn;
    rtfw_step_result_init_fn result_init_fn;
    rtfw_create_fn create_fn;
    rtfw_register_callback_fn register_fn;
    rtfw_lifecycle_fn finalize_fn;
    rtfw_lifecycle_fn start_fn;
    rtfw_step_fn step_fn;
    rtfw_lifecycle_fn stop_fn;
    rtfw_get_state_fn get_state_fn;
    rtfw_last_error_fn last_error_fn;
    rtfw_destroy_fn destroy_fn;

    LOAD_FUNCTION(config_init_fn, "rtfw_config_init");
    LOAD_FUNCTION(config_set_fn, "rtfw_config_set");
    LOAD_FUNCTION(capabilities_fn, "rt_query_capabilities");
    LOAD_FUNCTION(status_message_fn, "rtfw_status_message");
    LOAD_FUNCTION(frame_init_fn, "rtfw_frame_context_init");
    LOAD_FUNCTION(result_init_fn, "rtfw_step_result_init");
    LOAD_FUNCTION(create_fn, "rtfw_create");
    LOAD_FUNCTION(register_fn, "rtfw_register_callback");
    LOAD_FUNCTION(finalize_fn, "rtfw_finalize");
    LOAD_FUNCTION(start_fn, "rtfw_start");
    LOAD_FUNCTION(step_fn, "rtfw_step");
    LOAD_FUNCTION(stop_fn, "rtfw_stop");
    LOAD_FUNCTION(get_state_fn, "rtfw_get_state");
    LOAD_FUNCTION(last_error_fn, "rtfw_last_error");
    LOAD_FUNCTION(destroy_fn, "rtfw_destroy");

    {
        const rt_capabilities_c capabilities = capabilities_fn();
        if (capabilities.compiled_graph != 0 ||
            capabilities.host_driven_time != 1 ||
            capabilities.bounded_memory_plan != 0 ||
            strcmp(
                status_message_fn(RTFW_STATUS_INVALID_CONFIG),
                "invalid runtime configuration") != 0) {
            fprintf(stderr, "capability or status contract mismatch\n");
            lib_close(handle);
            return EXIT_FAILURE;
        }
    }

    rtfw_config config;
    config_init_fn(&config);
    if (config_set_fn(&config, "scratch_bytes", "64") != RTFW_STATUS_OK ||
        config_set_fn(&config, "callback_capacity", "1") != RTFW_STATUS_OK ||
        config_set_fn(&config, "unknown", "1") != RTFW_STATUS_INVALID_CONFIG) {
        fprintf(stderr, "strict configuration parsing failed\n");
        lib_close(handle);
        return EXIT_FAILURE;
    }
    config.reserved = 1;
    if (config_set_fn(&config, "scratch_bytes", "64") !=
            RTFW_STATUS_INVALID_ARGUMENT) {
        fprintf(stderr, "configuration accepted a nonzero reserved field\n");
        lib_close(handle);
        return EXIT_FAILURE;
    }
    config.reserved = 0;

    rtfw_handle *instance = NULL;
    if (create_fn(&config, &instance) != RTFW_STATUS_OK || !instance) {
        fprintf(stderr, "rtfw_create failed\n");
        lib_close(handle);
        return EXIT_FAILURE;
    }

    rtfw_frame_context frame;
    frame_init_fn(&frame);
    frame.delta_ns = 1000000;
    frame.reserved1[3] = 1;
    if (step_fn(instance, &frame, NULL) != RTFW_STATUS_INVALID_ARGUMENT ||
        strstr(last_error_fn(instance), "reserved") == NULL) {
        fprintf(stderr, "frame accepted a nonzero reserved field\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }
    frame.reserved1[3] = 0;
    if (step_fn(instance, &frame, NULL) != RTFW_STATUS_INVALID_STATE ||
        start_fn(instance) != RTFW_STATUS_INVALID_STATE) {
        fprintf(stderr, "lifecycle accepted an invalid transition\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }
    if (step_fn(NULL, &frame, NULL) != RTFW_STATUS_INVALID_ARGUMENT) {
        fprintf(stderr, "rtfw_step should reject NULL handles\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }

    callback_state state = {0, 0};
    if (register_fn(instance, "cabi.callback", test_callback, &state) !=
            RTFW_STATUS_OK) {
        fprintf(stderr, "runtime callback registration failed\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }
    if (register_fn(instance, "cabi.over-capacity", test_callback, &state) !=
            RTFW_STATUS_CAPACITY_EXCEEDED ||
        finalize_fn(instance) != RTFW_STATUS_OK ||
        start_fn(instance) != RTFW_STATUS_OK) {
        fprintf(stderr, "runtime setup failed\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }
    if (register_fn(instance, "cabi.too-late", test_callback, &state) !=
            RTFW_STATUS_INVALID_STATE ||
        strstr(last_error_fn(instance), "frozen") == NULL) {
        fprintf(stderr, "runtime accepted late callback registration\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }

    for (uint64_t i = 0; i < 5; ++i) {
        frame.frame_index = i;
        rtfw_step_result result;
        result_init_fn(&result);
        if (i == 0) {
            result.reserved1[4] = 1;
            if (step_fn(instance, &frame, &result) !=
                    RTFW_STATUS_INVALID_ARGUMENT) {
                fprintf(stderr, "result accepted a nonzero reserved field\n");
                destroy_fn(instance);
                lib_close(handle);
                return EXIT_FAILURE;
            }
            result_init_fn(&result);
        }
        rtfw_status status = step_fn(instance, &frame, &result);
        if (status != RTFW_STATUS_OK || result.callbacks_executed != 1) {
            fprintf(
                stderr,
                "step %llu failed with status %d\n",
                (unsigned long long)i,
                (int)status);
            destroy_fn(instance);
            lib_close(handle);
            return EXIT_FAILURE;
        }
    }

    rtfw_runtime_state runtime_state = RTFW_STATE_CONFIGURING;
    if (stop_fn(instance) != RTFW_STATUS_OK ||
        get_state_fn(instance, &runtime_state) != RTFW_STATUS_OK ||
        runtime_state != RTFW_STATE_STOPPED ||
        state.calls != 5 ||
        state.last_frame != 4) {
        fprintf(stderr, "runtime shutdown or callback validation failed\n");
        destroy_fn(instance);
        lib_close(handle);
        return EXIT_FAILURE;
    }

    destroy_fn(instance);
    destroy_fn(NULL);

    if (lib_close(handle) != 0) {
        fprintf(stderr, "failed to unload runtime: %s\n", lib_error());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
