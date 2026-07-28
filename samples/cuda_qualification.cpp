#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <cuda.h>

#include <rt/cuda_backend.hpp>
#include <rt/cuda_driver.hpp>
#include <rtfw/version.h>

namespace {

constexpr std::string_view kPtx = R"ptx(
.version 6.0
.target sm_50
.address_size 64

.visible .entry rtfw_add_one(
    .param .u64 rtfw_add_one_param_0,
    .param .u32 rtfw_add_one_param_1
)
{
    .reg .pred %p;
    .reg .b32 %r<5>;
    .reg .b64 %rd<4>;

    ld.param.u64 %rd1, [rtfw_add_one_param_0];
    ld.param.u32 %r1, [rtfw_add_one_param_1];
    mov.u32 %r2, %tid.x;
    mov.u32 %r3, %ctaid.x;
    mov.u32 %r4, %ntid.x;
    mad.lo.s32 %r2, %r3, %r4, %r2;
    setp.ge.u32 %p, %r2, %r1;
    @%p bra DONE;
    mul.wide.u32 %rd2, %r2, 4;
    add.s64 %rd3, %rd1, %rd2;
    ld.global.u32 %r3, [%rd3];
    add.u32 %r3, %r3, 1;
    st.global.u32 [%rd3], %r3;
DONE:
    ret;
}
)ptx";

struct StageSample {
    std::size_t iteration = 0;
    const char* stage = "";
    std::uint64_t submit_call_ns = 0;
    std::uint64_t completion_wait_ns = 0;
    std::uint64_t poll_call_ns = 0;
    std::uint64_t poll_count = 0;
};

bool cuda_ok(CUresult result, const char* operation) {
    if (result == CUDA_SUCCESS) {
        return true;
    }
    const char* name = "CUDA_ERROR_UNKNOWN";
    const char* description = "unknown CUDA error";
    (void)cuGetErrorName(result, &name);
    (void)cuGetErrorString(result, &description);
    std::cerr
        << operation << ": " << name << ": " << description << '\n';
    return false;
}

std::uint64_t elapsed_ns(
    std::chrono::steady_clock::time_point begin,
    std::chrono::steady_clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            end - begin)
            .count());
}

bool run_submission(
    rtfw_device_backend_api& api,
    rt::DeviceSubmission& submission,
    StageSample& sample) {
    const auto submit_begin = std::chrono::steady_clock::now();
    const auto submit_status =
        api.submit(api.instance, &submission);
    const auto submit_end = std::chrono::steady_clock::now();
    sample.submit_call_ns =
        elapsed_ns(submit_begin, submit_end);
    if (submit_status != RTFW_DEVICE_STATUS_OK) {
        std::cerr
            << sample.stage << " submit failed: "
            << submit_status << '\n';
        return false;
    }

    const auto wait_begin = submit_end;
    for (;;) {
        rtfw_device_completion completion{};
        std::uint64_t count = 0;
        const auto poll_begin = std::chrono::steady_clock::now();
        const auto poll_status = api.poll(
            api.instance,
            &completion,
            1,
            &count);
        const auto poll_end = std::chrono::steady_clock::now();
        sample.poll_call_ns += elapsed_ns(poll_begin, poll_end);
        ++sample.poll_count;
        if (poll_status != RTFW_DEVICE_STATUS_OK) {
            std::cerr
                << sample.stage << " poll failed: "
                << poll_status << '\n';
            return false;
        }
        if (count != 0) {
            sample.completion_wait_ns =
                elapsed_ns(wait_begin, poll_end);
            if (completion.submission_id !=
                    submission.submission_id ||
                completion.status != RTFW_DEVICE_STATUS_OK) {
                std::cerr
                    << sample.stage
                    << " completion failed: id="
                    << completion.submission_id
                    << " status=" << completion.status << '\n';
                return false;
            }
            return true;
        }
        std::this_thread::yield();
    }
}

bool run_backend(
    CUcontext context,
    CUstream stream,
    CUfunction function,
    std::size_t warmup_iterations,
    std::size_t iterations,
    std::vector<StageSample>& samples,
    rtfw_device_health& final_health) {
    const std::array<rt::CudaStream, 1> streams{
        reinterpret_cast<std::uintptr_t>(stream)};
    rt::CudaBackendConfig config{};
    config.queue_capacity = 8;
    config.buffer_capacity = 2;
    config.kernel_capacity = 2;
    config.context = reinterpret_cast<std::uintptr_t>(context);
    config.streams = streams;
    rt::CudaDeviceBackend backend(rt::cuda_driver_api(), config);
    std::uint64_t kernel_token = 0;
    if (backend.register_kernel(
            reinterpret_cast<std::uintptr_t>(function),
            kernel_token) != RTFW_DEVICE_STATUS_OK) {
        std::cerr << "kernel registration failed\n";
        return false;
    }
    auto api = backend.api();

    rtfw_device_init_config initialize{};
    initialize.struct_size = sizeof(initialize);
    initialize.abi_version = RTFW_DEVICE_ABI_VERSION;
    initialize.requested_in_flight = 8;
    initialize.requested_registered_buffers = 1;
    if (api.initialize(api.instance, &initialize) !=
        RTFW_DEVICE_STATUS_OK) {
        std::cerr << "backend initialization failed\n";
        return false;
    }

    constexpr std::size_t value_count = 4096;
    std::vector<std::uint32_t> values(value_count);
    rtfw_device_buffer_registration registration{};
    registration.struct_size = sizeof(registration);
    registration.flags =
        RTFW_DEVICE_BUFFER_HOST_READ |
        RTFW_DEVICE_BUFFER_HOST_WRITE |
        RTFW_DEVICE_BUFFER_DEVICE_READ |
        RTFW_DEVICE_BUFFER_DEVICE_WRITE;
    registration.data = values.data();
    registration.bytes = values.size() * sizeof(values[0]);
    std::memcpy(
        registration.name,
        "qualification.values",
        sizeof("qualification.values"));
    std::uint64_t buffer_token = 0;
    if (api.register_buffer(
            api.instance,
            &registration,
            &buffer_token) != RTFW_DEVICE_STATUS_OK) {
        std::cerr << "buffer registration failed\n";
        (void)api.shutdown(api.instance);
        return false;
    }

    bool passed = true;
    std::uint64_t submission_id = 1;
    samples.reserve(iterations * 3);
    const auto total_iterations = warmup_iterations + iterations;
    for (std::size_t run_iteration = 0;
         run_iteration < total_iterations && passed;
         ++run_iteration) {
        const bool recording =
            run_iteration >= warmup_iterations;
        const auto measurement_iteration =
            recording
            ? run_iteration - warmup_iterations
            : 0;
        for (std::size_t index = 0;
             index < values.size();
             ++index) {
            values[index] =
                static_cast<std::uint32_t>(
                    index + run_iteration);
        }

        auto upload = rt::make_device_submission();
        upload.submission_id = submission_id++;
        upload.timeout_ns = 5'000'000'000ull;
        upload.opcode =
            rt::cuda_device_opcode_copy_host_to_device;
        upload.buffer_count = 1;
        upload.buffers[0].buffer_token = buffer_token;
        upload.buffers[0].access = RTFW_DEVICE_ACCESS_READ;
        upload.buffers[0].bytes = registration.bytes;
        StageSample upload_sample{
            measurement_iteration,
            "host_to_device"};
        passed = run_submission(
            api,
            upload,
            upload_sample);
        if (recording) {
            samples.push_back(upload_sample);
        }
        if (!passed) {
            break;
        }

        rt::CudaKernelLaunch launch{};
        launch.kernel_token = kernel_token;
        launch.grid_x = static_cast<std::uint32_t>(
            (value_count + 255) / 256);
        launch.block_x = 256;
        passed =
            rt::cuda_kernel_add_buffer_argument(launch, 0);
        const auto count =
            static_cast<std::uint32_t>(value_count);
        passed = passed &&
            rt::cuda_kernel_add_scalar_argument(launch, count);
        auto kernel = rt::make_device_submission();
        kernel.submission_id = submission_id++;
        kernel.timeout_ns = 5'000'000'000ull;
        kernel.buffer_count = 1;
        kernel.buffers[0].buffer_token = buffer_token;
        kernel.buffers[0].access =
            RTFW_DEVICE_ACCESS_READ_WRITE;
        kernel.buffers[0].bytes = registration.bytes;
        rt::set_cuda_kernel_launch(kernel, launch);
        StageSample kernel_sample{
            measurement_iteration,
            "kernel"};
        passed = passed &&
            run_submission(api, kernel, kernel_sample);
        if (recording) {
            samples.push_back(kernel_sample);
        }
        if (!passed) {
            break;
        }

        std::fill(values.begin(), values.end(), 0);
        auto download = rt::make_device_submission();
        download.submission_id = submission_id++;
        download.timeout_ns = 5'000'000'000ull;
        download.opcode =
            rt::cuda_device_opcode_copy_device_to_host;
        download.buffer_count = 1;
        download.buffers[0].buffer_token = buffer_token;
        download.buffers[0].access = RTFW_DEVICE_ACCESS_WRITE;
        download.buffers[0].bytes = registration.bytes;
        StageSample download_sample{
            measurement_iteration,
            "device_to_host"};
        passed = run_submission(
            api,
            download,
            download_sample);
        if (recording) {
            samples.push_back(download_sample);
        }
        if (!passed) {
            break;
        }
        for (std::size_t index = 0;
             index < values.size();
             ++index) {
            const auto expected = static_cast<std::uint32_t>(
                index + run_iteration + 1);
            if (values[index] != expected) {
                std::cerr
                    << "validation failed at run iteration "
                    << run_iteration << ", index " << index
                    << ": expected " << expected
                    << ", got " << values[index] << '\n';
                passed = false;
                break;
            }
        }
    }

    final_health = rt::make_device_health();
    if (api.get_health(api.instance, &final_health) !=
        RTFW_DEVICE_STATUS_OK) {
        passed = false;
    }
    if (api.unregister_buffer(
            api.instance,
            buffer_token) != RTFW_DEVICE_STATUS_OK) {
        passed = false;
    }
    if (api.shutdown(api.instance) != RTFW_DEVICE_STATUS_OK) {
        passed = false;
    }
    return passed;
}

std::string json_escape(std::string_view input) {
    std::string result;
    result.reserve(input.size());
    for (const char value : input) {
        switch (value) {
        case '\\':
            result += "\\\\";
            break;
        case '"':
            result += "\\\"";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            result += value;
            break;
        }
    }
    return result;
}

const char* operating_system() noexcept {
#if defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#else
    return "other";
#endif
}

struct QualificationOptions {
    std::size_t warmup_iterations = 1000;
    std::size_t measurement_iterations = 1000;
};

bool parse_count(
    std::string_view value,
    std::size_t& output) {
    std::size_t parsed_value = 0;
    const auto parsed = std::from_chars(
        value.data(),
        value.data() + value.size(),
        parsed_value);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != value.data() + value.size() ||
        parsed_value == 0 ||
        parsed_value > 1'000'000) {
        return false;
    }
    output = parsed_value;
    return true;
}

bool parse_options(
    int argc,
    char** argv,
    QualificationOptions& options) {
    if ((argc - 1) % 2 != 0) {
        return false;
    }
    bool saw_warmup = false;
    bool saw_measurement = false;
    for (int index = 1; index < argc; index += 2) {
        const std::string_view option(argv[index]);
        const std::string_view value(argv[index + 1]);
        if (option == "--warmup" && !saw_warmup) {
            if (!parse_count(value, options.warmup_iterations)) {
                return false;
            }
            saw_warmup = true;
        } else if (
            option == "--iterations" &&
            !saw_measurement) {
            if (!parse_count(
                    value,
                    options.measurement_iterations)) {
                return false;
            }
            saw_measurement = true;
        } else {
            return false;
        }
    }
    return true;
}

void write_json(
    const char* device_name,
    const char* pci_bus_id,
    int driver_version,
    int compute_major,
    int compute_minor,
    const QualificationOptions& options,
    const std::vector<StageSample>& samples,
    const rtfw_device_health& health) {
    std::cout
        << "{\n"
        << "  \"schema_version\": 1,\n"
        << "  \"result\": \"pass\",\n"
        << "  \"qualification_claim\": \"evidence_only\",\n"
        << "  \"runtime_version\": \"" RTFW_VERSION_STRING "\",\n"
        << "  \"backend_id\": \"rtfw.cuda.driver.v1\",\n"
        << "  \"os\": \"" << operating_system() << "\",\n"
        << "  \"cuda_toolkit_version\": " << CUDA_VERSION << ",\n"
        << "  \"cuda_driver_version\": " << driver_version << ",\n"
        << "  \"device_name\": \""
        << json_escape(device_name) << "\",\n"
        << "  \"pci_bus_id\": \""
        << json_escape(pci_bus_id) << "\",\n"
        << "  \"compute_capability\": \""
        << compute_major << '.' << compute_minor << "\",\n"
        << "  \"warmup_iterations\": "
        << options.warmup_iterations << ",\n"
        << "  \"measurement_iterations\": "
        << options.measurement_iterations << ",\n"
        << "  \"health\": {\"submissions\": "
        << health.submissions
        << ", \"completions\": " << health.completions
        << ", \"timeouts\": " << health.timeouts
        << ", \"errors\": " << health.errors
        << ", \"losses\": " << health.losses << "},\n"
        << "  \"samples\": [\n";
    for (std::size_t index = 0;
         index < samples.size();
         ++index) {
        const auto& sample = samples[index];
        std::cout
            << "    {\"iteration\": " << sample.iteration
            << ", \"stage\": \"" << sample.stage
            << "\", \"submit_call_ns\": "
            << sample.submit_call_ns
            << ", \"completion_wait_ns\": "
            << sample.completion_wait_ns
            << ", \"poll_call_ns\": "
            << sample.poll_call_ns
            << ", \"poll_count\": "
            << sample.poll_count << '}';
        std::cout << (index + 1 == samples.size() ? "\n" : ",\n");
    }
    std::cout << "  ]\n}\n";
}

bool cleanup_cuda(
    CUcontext context,
    CUstream& stream,
    CUmodule& module,
    CUdevice device) {
    if (!context) {
        return true;
    }
    bool cleanup_ok = cuda_ok(
        cuCtxPushCurrent(context),
        "cleanup cuCtxPushCurrent");
    if (cleanup_ok) {
        if (module) {
            const bool unloaded = cuda_ok(
                cuModuleUnload(module),
                "cuModuleUnload");
            cleanup_ok = unloaded && cleanup_ok;
            if (unloaded) {
                module = nullptr;
            }
        }
        if (stream) {
            const bool destroyed = cuda_ok(
                cuStreamDestroy(stream),
                "cuStreamDestroy");
            cleanup_ok = destroyed && cleanup_ok;
            if (destroyed) {
                stream = nullptr;
            }
        }
        CUcontext popped = nullptr;
        const bool popped_ok = cuda_ok(
            cuCtxPopCurrent(&popped),
            "cleanup cuCtxPopCurrent");
        cleanup_ok =
            popped_ok && popped == context && cleanup_ok;
        if (popped_ok && popped != context) {
            std::cerr
                << "cleanup cuCtxPopCurrent returned "
                   "an unexpected context\n";
        }
    }
    cleanup_ok =
        cuda_ok(
            cuDevicePrimaryCtxRelease(device),
            "cuDevicePrimaryCtxRelease") &&
        cleanup_ok;
    return cleanup_ok;
}

} // namespace

int main(int argc, char** argv) {
    QualificationOptions options{};
    if (!parse_options(argc, argv, options)) {
        std::cerr
            << "usage: sample_cuda_qualification "
               "[--warmup 1..1000000] "
               "[--iterations 1..1000000]\n";
        return 2;
    }
    if (!cuda_ok(cuInit(0), "cuInit")) {
        return 1;
    }

    CUdevice device = 0;
    CUcontext context = nullptr;
    CUstream stream = nullptr;
    CUmodule module = nullptr;
    CUfunction function = nullptr;
    if (!cuda_ok(cuDeviceGet(&device, 0), "cuDeviceGet") ||
        !cuda_ok(
            cuDevicePrimaryCtxRetain(&context, device),
            "cuDevicePrimaryCtxRetain") ||
        !cuda_ok(cuCtxPushCurrent(context), "cuCtxPushCurrent")) {
        if (context) {
            (void)cuDevicePrimaryCtxRelease(device);
        }
        return 1;
    }

    bool setup_ok =
        cuda_ok(
            cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING),
            "cuStreamCreate") &&
        cuda_ok(
            cuModuleLoadData(&module, kPtx.data()),
            "cuModuleLoadData") &&
        cuda_ok(
            cuModuleGetFunction(
                &function,
                module,
                "rtfw_add_one"),
            "cuModuleGetFunction");
    CUcontext popped = nullptr;
    setup_ok =
        cuda_ok(cuCtxPopCurrent(&popped), "cuCtxPopCurrent") &&
        popped == context &&
        setup_ok;
    if (!setup_ok) {
        (void)cleanup_cuda(context, stream, module, device);
        return 1;
    }

    char device_name[256]{};
    char pci_bus_id[32]{};
    int driver_version = 0;
    int compute_major = 0;
    int compute_minor = 0;
    if (!cuda_ok(
            cuDeviceGetName(
                device_name,
                static_cast<int>(sizeof(device_name)),
                device),
            "cuDeviceGetName") ||
        !cuda_ok(
            cuDeviceGetPCIBusId(
                pci_bus_id,
                static_cast<int>(sizeof(pci_bus_id)),
                device),
            "cuDeviceGetPCIBusId") ||
        !cuda_ok(
            cuDriverGetVersion(&driver_version),
            "cuDriverGetVersion") ||
        !cuda_ok(
            cuDeviceGetAttribute(
                &compute_major,
                CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR,
                device),
            "compute capability major") ||
        !cuda_ok(
            cuDeviceGetAttribute(
                &compute_minor,
                CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR,
                device),
            "compute capability minor")) {
        (void)cleanup_cuda(context, stream, module, device);
        return 1;
    }

    std::vector<StageSample> samples;
    auto health = rt::make_device_health();
    const bool passed = run_backend(
        context,
        stream,
        function,
        options.warmup_iterations,
        options.measurement_iterations,
        samples,
        health);

    const bool cleanup_ok =
        cleanup_cuda(context, stream, module, device);
    if (!passed || !cleanup_ok) {
        return 1;
    }

    write_json(
        device_name,
        pci_bus_id,
        driver_version,
        compute_major,
        compute_minor,
        options,
        samples,
        health);
    return 0;
}
