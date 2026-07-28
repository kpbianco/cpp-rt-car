#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <rt/xdma_backend.hpp>
#include <rt/xdma_linux.hpp>
#include <rtfw/version.h>

namespace {

constexpr std::size_t kPageBytes = 4096;

struct Options {
    std::string h2c_path = "/dev/xdma0_h2c_0";
    std::string c2h_path = "/dev/xdma0_c2h_0";
    std::string pci_bdf;
    std::string driver_id;
    std::string bitstream_id;
    std::uint64_t device_offset = 0;
    std::size_t bytes = kPageBytes;
    std::size_t warmup = 1000;
    std::size_t iterations = 10000;
};

struct Sample {
    std::size_t iteration = 0;
    const char* direction = "";
    std::uint64_t submit_call_ns = 0;
    std::uint64_t completion_wait_ns = 0;
    std::uint64_t poll_call_ns = 0;
    std::uint64_t poll_count = 0;
};

bool parse_unsigned(std::string_view value, std::uint64_t& output) {
    const auto* begin = value.data();
    const auto* end = begin + value.size();
    const auto result = std::from_chars(begin, end, output);
    return result.ec == std::errc{} && result.ptr == end;
}

bool parse_size(std::string_view value, std::size_t& output) {
    std::uint64_t parsed = 0;
    if (!parse_unsigned(value, parsed) ||
        parsed > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    output = static_cast<std::size_t>(parsed);
    return true;
}

bool parse_options(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (index + 1 >= argc) {
            return false;
        }
        const std::string_view value = argv[++index];
        if (option == "--h2c") {
            options.h2c_path = value;
        } else if (option == "--c2h") {
            options.c2h_path = value;
        } else if (option == "--pci-bdf") {
            options.pci_bdf = value;
        } else if (option == "--driver-id") {
            options.driver_id = value;
        } else if (option == "--bitstream-id") {
            options.bitstream_id = value;
        } else if (option == "--device-offset") {
            if (!parse_unsigned(value, options.device_offset)) {
                return false;
            }
        } else if (option == "--bytes") {
            if (!parse_size(value, options.bytes)) {
                return false;
            }
        } else if (option == "--warmup") {
            if (!parse_size(value, options.warmup)) {
                return false;
            }
        } else if (option == "--iterations") {
            if (!parse_size(value, options.iterations)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return !options.h2c_path.empty() &&
           !options.c2h_path.empty() &&
           !options.pci_bdf.empty() &&
           !options.driver_id.empty() &&
           !options.bitstream_id.empty() &&
           options.bytes != 0 &&
           options.bytes % kPageBytes == 0 &&
           options.warmup <= 1'000'000 &&
           options.iterations != 0 &&
           options.iterations <= 1'000'000 &&
           options.device_offset % kPageBytes == 0;
}

std::uint64_t elapsed_ns(
    std::chrono::steady_clock::time_point begin,
    std::chrono::steady_clock::time_point end) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
            .count());
}

rtfw_device_submission make_submission(
    std::uint64_t id,
    std::uint64_t token,
    rt::XdmaDirection direction,
    const Options& options) {
    rtfw_device_submission submission{};
    submission.struct_size = sizeof(submission);
    submission.abi_version = RTFW_DEVICE_ABI_VERSION;
    submission.submission_id = id;
    submission.timeout_ns = 10'000'000'000ull;
    submission.buffer_count = 1;
    submission.buffers[0].buffer_token = token;
    submission.buffers[0].access =
        direction == rt::XdmaDirection::host_to_card
        ? RTFW_DEVICE_ACCESS_READ
        : RTFW_DEVICE_ACCESS_WRITE;
    submission.buffers[0].bytes = options.bytes;
    rt::XdmaTransfer transfer{};
    transfer.device_offset = options.device_offset;
    rt::set_xdma_transfer(submission, direction, transfer);
    return submission;
}

bool run_submission(
    rtfw_device_backend_api& api,
    rtfw_device_submission& submission,
    Sample& sample) {
    const auto submit_begin = std::chrono::steady_clock::now();
    const auto submit_status = api.submit(api.instance, &submission);
    const auto submit_end = std::chrono::steady_clock::now();
    sample.submit_call_ns = elapsed_ns(submit_begin, submit_end);
    if (submit_status != RTFW_DEVICE_STATUS_OK) {
        std::cerr << "submit failed: " << submit_status << '\n';
        return false;
    }

    const auto wait_begin = submit_end;
    for (;;) {
        rtfw_device_completion completion{};
        std::uint64_t count = 0;
        const auto poll_begin = std::chrono::steady_clock::now();
        const auto poll_status =
            api.poll(api.instance, &completion, 1, &count);
        const auto poll_end = std::chrono::steady_clock::now();
        sample.poll_call_ns += elapsed_ns(poll_begin, poll_end);
        ++sample.poll_count;
        if (poll_status != RTFW_DEVICE_STATUS_OK) {
            std::cerr << "poll failed: " << poll_status << '\n';
            return false;
        }
        if (count != 0) {
            sample.completion_wait_ns = elapsed_ns(wait_begin, poll_end);
            if (completion.submission_id != submission.submission_id ||
                completion.status != RTFW_DEVICE_STATUS_OK ||
                completion.value != submission.buffers[0].bytes) {
                std::cerr
                    << "completion failed: id="
                    << completion.submission_id
                    << " status=" << completion.status
                    << " bytes=" << completion.value << '\n';
                return false;
            }
            return true;
        }
        std::this_thread::yield();
    }
}

void write_json_string(std::ostream& output, std::string_view value) {
    output << '"';
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20u) {
                output
                    << "\\u"
                    << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned>(character)
                    << std::dec << std::setfill(' ');
            } else {
                output << static_cast<char>(character);
            }
            break;
        }
    }
    output << '"';
}

void write_evidence(
    const Options& options,
    const std::vector<Sample>& samples,
    const rtfw_device_health& health) {
    std::cout
        << "{\n"
        << "  \"schema_version\": 1,\n"
        << "  \"result\": \"pass\",\n"
        << "  \"qualification_claim\": \"evidence_only\",\n";
    std::cout << "  \"runtime_version\": ";
    write_json_string(std::cout, RTFW_VERSION_STRING);
    std::cout << ",\n  \"backend_id\": "
              << "\"rtfw.xdma.xilinx_linux_aximm.v1\",\n";
    const std::array<std::pair<std::string_view, std::string_view>, 5>
        strings{{
            {"pci_bdf", options.pci_bdf},
            {"driver_id", options.driver_id},
            {"bitstream_id", options.bitstream_id},
            {"h2c_path", options.h2c_path},
            {"c2h_path", options.c2h_path},
        }};
    for (const auto& [name, value] : strings) {
        std::cout << "  \"" << name << "\": ";
        write_json_string(std::cout, value);
        std::cout << ",\n";
    }
    std::cout
        << "  \"device_offset\": " << options.device_offset << ",\n"
        << "  \"transfer_bytes\": " << options.bytes << ",\n"
        << "  \"warmup_iterations\": " << options.warmup << ",\n"
        << "  \"measurement_iterations\": "
        << options.iterations << ",\n"
        << "  \"health\": {\"submissions\": " << health.submissions
        << ", \"completions\": " << health.completions
        << ", \"timeouts\": " << health.timeouts
        << ", \"errors\": " << health.errors
        << ", \"losses\": " << health.losses << "},\n"
        << "  \"samples\": [\n";
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const auto& sample = samples[index];
        std::cout
            << "    {\"iteration\": " << sample.iteration
            << ", \"direction\": \"" << sample.direction
            << "\", \"submit_call_ns\": " << sample.submit_call_ns
            << ", \"completion_wait_ns\": "
            << sample.completion_wait_ns
            << ", \"poll_call_ns\": " << sample.poll_call_ns
            << ", \"poll_count\": " << sample.poll_count << '}';
        std::cout << (index + 1 == samples.size() ? "\n" : ",\n");
    }
    std::cout << "  ]\n}\n";
}

} // namespace

int main(int argc, char** argv) {
    Options options{};
    if (!parse_options(argc, argv, options)) {
        std::cerr
            << "usage: sample_xdma_qualification "
               "--pci-bdf ID --driver-id ID --bitstream-id ID "
               "[--h2c PATH] [--c2h PATH] [--device-offset BYTES] "
               "[--bytes MULTIPLE_OF_4096] [--warmup N] "
               "[--iterations N]\n";
        return 2;
    }

    const std::array<std::string_view, 1> h2c_paths{options.h2c_path};
    const std::array<std::string_view, 1> c2h_paths{options.c2h_path};
    rt::LinuxXdmaConfig driver_config{};
    driver_config.h2c_paths = h2c_paths;
    driver_config.c2h_paths = c2h_paths;
    rt::LinuxXdmaDriver driver(driver_config);

    rt::XdmaBackendConfig backend_config{};
    backend_config.queue_capacity = 8;
    backend_config.buffer_capacity = 1;
    backend_config.worker_count = 1;
    backend_config.h2c_channel_count = 1;
    backend_config.c2h_channel_count = 1;
    backend_config.max_transfer_bytes = options.bytes;
    backend_config.max_buffer_bytes = options.bytes;
    backend_config.transfer_alignment = kPageBytes;
    rt::XdmaDeviceBackend backend(driver.api(), backend_config);
    auto api = backend.api();

    rtfw_device_init_config initialize{};
    initialize.struct_size = sizeof(initialize);
    initialize.abi_version = RTFW_DEVICE_ABI_VERSION;
    initialize.requested_in_flight = 8;
    initialize.requested_registered_buffers = 1;
    if (api.initialize(api.instance, &initialize) !=
        RTFW_DEVICE_STATUS_OK) {
        std::cerr << "backend initialization failed\n";
        return 1;
    }

    using Storage = std::unique_ptr<std::byte, decltype(&std::free)>;
    Storage storage(
        static_cast<std::byte*>(
            std::aligned_alloc(kPageBytes, options.bytes)),
        &std::free);
    if (!storage) {
        std::cerr << "aligned host allocation failed\n";
        (void)api.shutdown(api.instance);
        return 1;
    }

    rtfw_device_buffer_registration registration{};
    registration.struct_size = sizeof(registration);
    registration.flags =
        RTFW_DEVICE_BUFFER_HOST_READ |
        RTFW_DEVICE_BUFFER_HOST_WRITE |
        RTFW_DEVICE_BUFFER_DEVICE_READ |
        RTFW_DEVICE_BUFFER_DEVICE_WRITE;
    registration.data = storage.get();
    registration.bytes = options.bytes;
    std::memcpy(
        registration.name,
        "qualification.xdma",
        sizeof("qualification.xdma"));
    std::uint64_t token = 0;
    if (api.register_buffer(
            api.instance,
            &registration,
            &token) != RTFW_DEVICE_STATUS_OK) {
        std::cerr << "buffer registration failed\n";
        (void)api.shutdown(api.instance);
        return 1;
    }

    std::vector<std::byte> expected(options.bytes);
    std::vector<Sample> samples;
    samples.reserve(options.iterations * 2);
    std::uint64_t submission_id = 1;
    bool passed = true;
    const auto total = options.warmup + options.iterations;
    for (std::size_t iteration = 0;
         iteration < total && passed;
         ++iteration) {
        for (std::size_t index = 0; index < options.bytes; ++index) {
            expected[index] = static_cast<std::byte>(
                (index + iteration * 17u) & 0xffu);
        }
        std::memcpy(storage.get(), expected.data(), options.bytes);
        Sample upload{};
        upload.iteration = iteration - std::min(iteration, options.warmup);
        upload.direction = "h2c";
        auto upload_submission = make_submission(
            submission_id++,
            token,
            rt::XdmaDirection::host_to_card,
            options);
        passed = run_submission(api, upload_submission, upload);
        std::memset(storage.get(), 0, options.bytes);
        Sample download{};
        download.iteration = upload.iteration;
        download.direction = "c2h";
        auto download_submission = make_submission(
            submission_id++,
            token,
            rt::XdmaDirection::card_to_host,
            options);
        passed =
            passed && run_submission(api, download_submission, download);
        passed =
            passed &&
            std::equal(
                expected.begin(),
                expected.end(),
                storage.get());
        if (iteration >= options.warmup) {
            samples.push_back(upload);
            samples.push_back(download);
        }
    }

    rtfw_device_health health{};
    health.struct_size = sizeof(health);
    passed =
        api.get_health(api.instance, &health) ==
            RTFW_DEVICE_STATUS_OK &&
        passed;
    passed =
        api.unregister_buffer(api.instance, token) ==
            RTFW_DEVICE_STATUS_OK &&
        passed;
    passed =
        api.shutdown(api.instance) == RTFW_DEVICE_STATUS_OK &&
        passed;
    if (!passed) {
        std::cerr << "XDMA qualification functional check failed\n";
        return 1;
    }
    write_evidence(options, samples, health);
    return 0;
}
