#include <rt/profile.hpp>
#include <rt/runtime.hpp>
#include <rtfw/version.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr std::chrono::nanoseconds kFramePeriod{1'000'000};
constexpr std::size_t kSystemCount = 4;
constexpr std::size_t kBodiesPerSystem = 2048;
constexpr std::size_t kTaskGrain = 128;
constexpr std::size_t kMaximumFrames = 3'600'000;

struct Options {
    std::string profile_path;
    std::chrono::nanoseconds warmup{0};
    std::chrono::nanoseconds run{1s};
    std::optional<std::size_t> worker_override;
    std::uint64_t seed = 1;
    bool self_paced = false;
    bool metrics_json = false;
    bool show_help = false;
    bool show_version = false;
};

void print_help(const char* executable) {
    std::cout
        << "Usage: " << executable << " --config <profile.json> [options]\n\n"
        << "Options:\n"
        << "  --config, --profile <file>  Complete RTFW runtime profile.\n"
        << "  --run <duration>            Measured duration (default: 1s).\n"
        << "  --warmup <duration>         In-process warmup duration.\n"
        << "  --threads <n>               Explicit worker-count override.\n"
        << "  --seed <n>                  Deterministic workload seed.\n"
        << "  --rt                        Use the runtime-owned periodic loop.\n"
        << "  --metrics-json              Emit one JSON metrics object.\n"
        << "  --metrics-json-interval     Alias used by the autotuner.\n"
        << "  --version                   Print the RTFW version.\n"
        << "  --help, -h                  Show this help message.\n\n"
        << "If --config is omitted, RTFW_PROFILE supplies the profile path. "
        << "The command line takes precedence.\n";
}

bool parse_unsigned(std::string_view text, std::uint64_t& output) noexcept {
    if (text.empty()) {
        return false;
    }
    std::uint64_t candidate = 0;
    const auto result = std::from_chars(
        text.data(),
        text.data() + text.size(),
        candidate,
        10);
    if (result.ec != std::errc{} ||
        result.ptr != text.data() + text.size()) {
        return false;
    }
    output = candidate;
    return true;
}

bool parse_positive_size(
    std::string_view text,
    std::size_t& output) noexcept {
    std::uint64_t candidate = 0;
    if (!parse_unsigned(text, candidate) ||
        candidate == 0 ||
        candidate > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    output = static_cast<std::size_t>(candidate);
    return true;
}

bool parse_duration(
    std::string_view text,
    std::chrono::nanoseconds& output) noexcept {
    std::uint64_t multiplier = 0;
    if (text.ends_with("ms")) {
        multiplier = 1'000'000;
        text.remove_suffix(2);
    } else if (text.ends_with("us")) {
        multiplier = 1'000;
        text.remove_suffix(2);
    } else if (text.ends_with("ns")) {
        multiplier = 1;
        text.remove_suffix(2);
    } else if (text.ends_with("s")) {
        multiplier = 1'000'000'000;
        text.remove_suffix(1);
    } else {
        return false;
    }
    if (text.empty()) {
        return false;
    }

    long double value = 0.0L;
    long double fraction_scale = 0.1L;
    bool decimal = false;
    bool digit = false;
    for (const char character : text) {
        if (character == '.') {
            if (decimal) {
                return false;
            }
            decimal = true;
            continue;
        }
        if (character < '0' || character > '9') {
            return false;
        }
        digit = true;
        const auto numeric = static_cast<unsigned int>(character - '0');
        if (decimal) {
            value += static_cast<long double>(numeric) * fraction_scale;
            fraction_scale *= 0.1L;
        } else {
            value = value * 10.0L + static_cast<long double>(numeric);
        }
    }
    const long double nanoseconds =
        value * static_cast<long double>(multiplier);
    if (!digit || value <= 0.0L ||
        !std::isfinite(nanoseconds) ||
        nanoseconds >
            static_cast<long double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(1h)
                    .count())) {
        return false;
    }
    const auto rounded = static_cast<std::uint64_t>(nanoseconds + 0.5L);
    if (rounded == 0 ||
        rounded >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
        return false;
    }
    output = std::chrono::nanoseconds{
        static_cast<std::int64_t>(rounded)};
    return true;
}

bool parse_options(int argc, char** argv, Options& options) {
    bool profile_from_cli = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            options.show_help = true;
        } else if (argument == "--version") {
            options.show_version = true;
        } else if (argument == "--rt") {
            options.self_paced = true;
        } else if (
            argument == "--metrics-json" ||
            argument == "--metrics-json-interval") {
            options.metrics_json = true;
        } else if (
            argument == "--config" ||
            argument == "--profile") {
            if (profile_from_cli || index + 1 >= argc) {
                std::cerr << argument
                          << " requires exactly one profile path\n";
                return false;
            }
            options.profile_path = argv[++index];
            profile_from_cli = true;
        } else if (argument == "--run") {
            if (index + 1 >= argc ||
                !parse_duration(argv[++index], options.run)) {
                std::cerr
                    << "--run requires a duration such as 500ms or 2s\n";
                return false;
            }
        } else if (argument == "--warmup") {
            if (index + 1 >= argc ||
                !parse_duration(argv[++index], options.warmup)) {
                std::cerr
                    << "--warmup requires a duration such as 500ms or 2s\n";
                return false;
            }
        } else if (argument == "--threads") {
            std::size_t workers = 0;
            if (index + 1 >= argc ||
                !parse_positive_size(argv[++index], workers)) {
                std::cerr << "--threads requires a positive integer\n";
                return false;
            }
            options.worker_override = workers;
        } else if (argument == "--seed") {
            if (index + 1 >= argc ||
                !parse_unsigned(argv[++index], options.seed)) {
                std::cerr << "--seed requires an unsigned integer\n";
                return false;
            }
        } else {
            std::cerr << "Unknown option: " << argument << '\n';
            return false;
        }
    }

    if (!profile_from_cli) {
        const char* environment_profile = std::getenv("RTFW_PROFILE");
        if (environment_profile != nullptr && environment_profile[0] != '\0') {
            options.profile_path = environment_profile;
        }
    }
    return true;
}

bool read_profile_file(
    const std::string& path,
    std::string& contents) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "Unable to open runtime profile: " << path << '\n';
        return false;
    }
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    const auto byte_count = static_cast<std::streamoff>(end);
    if (byte_count < 0 ||
        static_cast<std::uint64_t>(byte_count) >
            static_cast<std::uint64_t>(rt::runtime_profile_max_bytes)) {
        std::cerr << "Runtime profile exceeds "
                  << rt::runtime_profile_max_bytes << " bytes\n";
        return false;
    }
    input.seekg(0, std::ios::beg);
    contents.resize(static_cast<std::size_t>(byte_count));
    if (!contents.empty()) {
        input.read(
            contents.data(),
            static_cast<std::streamsize>(contents.size()));
        if (!input) {
            std::cerr << "Unable to read complete runtime profile: "
                      << path << '\n';
            return false;
        }
    }
    return true;
}

std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    state += 0x9e3779b97f4a7c15ull;
    std::uint64_t value = state;
    value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31u);
}

double unit_value(std::uint64_t& state) noexcept {
    constexpr double scale = 1.0 / 9'007'199'254'740'992.0;
    return static_cast<double>(splitmix64(state) >> 11u) * scale;
}

struct BodySystem {
    std::vector<double> position;
    std::vector<double> velocity;
    std::vector<double> acceleration;
    double delta_seconds = 0.0;

    void initialize(
        std::size_t count,
        std::uint64_t seed,
        std::size_t system_index) {
        position.resize(count);
        velocity.resize(count);
        acceleration.resize(count);
        std::uint64_t state =
            seed ^ (0xd6e8feb86659fd93ull *
                    static_cast<std::uint64_t>(system_index + 1));
        for (std::size_t index = 0; index < count; ++index) {
            position[index] = unit_value(state) * 100.0;
            velocity[index] = unit_value(state) * 2.0 - 1.0;
            acceleration[index] = unit_value(state) * 0.2 - 0.1;
        }
    }
};

rt::TaskResult integrate_range(
    void* user_data,
    const rt::TaskContext&,
    const rt::TaskRange& range) {
    auto& system = *static_cast<BodySystem*>(user_data);
    const double substep = system.delta_seconds / 8.0;
    for (std::size_t index = range.begin; index < range.end; ++index) {
        double position = system.position[index];
        double velocity = system.velocity[index];
        const double acceleration = system.acceleration[index];
        for (std::size_t iteration = 0; iteration < 8; ++iteration) {
            velocity += acceleration * substep;
            position += velocity * substep;
        }
        system.velocity[index] = velocity;
        system.position[index] = position;
    }
    return rt::TaskResult::ok;
}

rt::CallbackResult integrate_system(
    void* user_data,
    const rt::CallbackContext& context) {
    auto& system = *static_cast<BodySystem*>(user_data);
    system.delta_seconds =
        static_cast<double>(context.frame.delta.count()) / 1'000'000'000.0;
    const auto status = context.tasks.parallel_for(
        system.position.size(),
        kTaskGrain,
        &integrate_range,
        &system);
    return status == rt::Status::ok
        ? rt::CallbackResult::ok
        : rt::CallbackResult::error;
}

struct BarrierState {
    std::array<BodySystem*, kSystemCount> systems{};
    double checksum = 0.0;
    std::uint64_t frames = 0;
};

rt::CallbackResult verify_barrier(
    void* user_data,
    const rt::CallbackContext&) {
    auto& barrier = *static_cast<BarrierState*>(user_data);
    double checksum = 0.0;
    for (const auto* system : barrier.systems) {
        if (system == nullptr) {
            return rt::CallbackResult::error;
        }
        for (const double position : system->position) {
            checksum += position;
        }
    }
    if (!std::isfinite(checksum)) {
        return rt::CallbackResult::error;
    }
    barrier.checksum = checksum;
    ++barrier.frames;
    return rt::CallbackResult::ok;
}

struct MeasurementBuffer {
    std::uint64_t* samples = nullptr;
    std::size_t capacity = 0;
    std::size_t count = 0;
    std::size_t missed_frames = 0;
    std::size_t watchdog_trips = 0;
};

rt::CallbackResult record_periodic_frame(
    void* user_data,
    const rt::PeriodicFrameResult& frame) {
    auto& measurements = *static_cast<MeasurementBuffer*>(user_data);
    if (measurements.count >= measurements.capacity ||
        frame.finish_ns < frame.start_ns) {
        return rt::CallbackResult::error;
    }
    measurements.samples[measurements.count++] =
        frame.finish_ns - frame.start_ns;
    if (frame.deadline_missed) {
        ++measurements.missed_frames;
    }
    if (frame.watchdog_fired) {
        ++measurements.watchdog_trips;
    }
    return rt::CallbackResult::ok;
}

std::size_t frame_count_for(std::chrono::nanoseconds duration) {
    const auto duration_ns = static_cast<std::uint64_t>(duration.count());
    const auto period_ns = static_cast<std::uint64_t>(kFramePeriod.count());
    const auto frames = (duration_ns + period_ns - 1) / period_ns;
    if (frames == 0 || frames > kMaximumFrames) {
        return 0;
    }
    return static_cast<std::size_t>(frames);
}

bool report_status(
    std::string_view operation,
    rt::Status status,
    const rt::Runtime& runtime) {
    if (status == rt::Status::ok) {
        return true;
    }
    std::cerr << operation << " failed: " << rt::status_message(status);
    if (!runtime.last_error().empty()) {
        std::cerr << " (" << runtime.last_error() << ')';
    }
    std::cerr << '\n';
    return false;
}

bool build_workload(
    rt::Runtime& runtime,
    std::array<BodySystem, kSystemCount>& systems,
    BarrierState& barrier) {
    constexpr std::array<std::string_view, kSystemCount> phase_names{
        "physics.0", "physics.1", "physics.2", "physics.3"};
    constexpr std::array<std::string_view, kSystemCount> resource_names{
        "state.0", "state.1", "state.2", "state.3"};

    std::array<rt::PhaseHandle, kSystemCount> phases{};
    std::array<rt::ResourceHandle, kSystemCount> resources{};
    for (std::size_t index = 0; index < kSystemCount; ++index) {
        if (!report_status(
                "register physics phase",
                runtime.register_callback(
                    {
                        phase_names[index],
                        &integrate_system,
                        &systems[index],
                    },
                    phases[index]),
                runtime) ||
            !report_status(
                "register physics resource",
                runtime.register_resource(
                    resource_names[index],
                    resources[index]),
                runtime) ||
            !report_status(
                "declare physics resource access",
                runtime.declare_resource_access(
                    phases[index],
                    resources[index],
                    rt::ResourceAccess::write),
                runtime)) {
            return false;
        }
        barrier.systems[index] = &systems[index];
    }

    rt::PhaseHandle barrier_phase;
    if (!report_status(
            "register dependency barrier",
            runtime.register_callback(
                {"physics.barrier", &verify_barrier, &barrier},
                barrier_phase),
            runtime)) {
        return false;
    }
    for (std::size_t index = 0; index < kSystemCount; ++index) {
        if (!report_status(
                "add barrier dependency",
                runtime.add_dependency(phases[index], barrier_phase),
                runtime) ||
            !report_status(
                "declare barrier resource access",
                runtime.declare_resource_access(
                    barrier_phase,
                    resources[index],
                    rt::ResourceAccess::read),
                runtime)) {
            return false;
        }
    }
    return true;
}

struct RunMeasurements {
    std::vector<std::uint64_t> frame_ns;
    std::size_t missed_frames = 0;
    std::size_t watchdog_trips = 0;
    std::uint64_t trace_events_dropped = 0;
};

bool run_host_frames(
    rt::Runtime& runtime,
    std::uint64_t first_frame,
    std::size_t frame_count,
    RunMeasurements* measurements) {
    const auto period_ns = static_cast<std::uint64_t>(kFramePeriod.count());
    for (std::size_t index = 0; index < frame_count; ++index) {
        const auto now = runtime.now_ns();
        if (now > std::numeric_limits<std::uint64_t>::max() - period_ns) {
            std::cerr << "Host-driven deadline overflow\n";
            return false;
        }
        rt::StepResult result;
        const auto status = runtime.step(
            {
                first_frame + static_cast<std::uint64_t>(index),
                kFramePeriod,
                now + period_ns,
            },
            &result);
        if (!report_status("host-driven step", status, runtime) ||
            result.finish_ns < result.start_ns) {
            return false;
        }
        if (measurements != nullptr) {
            measurements->frame_ns[index] =
                result.finish_ns - result.start_ns;
            if (result.deadline_missed) {
                ++measurements->missed_frames;
            }
            if (result.watchdog_fired) {
                ++measurements->watchdog_trips;
            }
        }
    }
    return true;
}

bool run_periodic_frames(
    rt::Runtime& runtime,
    std::uint64_t first_frame,
    std::size_t frame_count,
    RunMeasurements* measurements) {
    rt::PeriodicRunConfig config;
    config.first_frame_index = first_frame;
    config.frame_count = frame_count;
    config.period = kFramePeriod;
    config.relative_deadline = kFramePeriod;
    rt::PeriodicRunResult result;

    MeasurementBuffer buffer;
    rt::PeriodicFrameObserver observer = nullptr;
    void* observer_data = nullptr;
    if (measurements != nullptr) {
        buffer.samples = measurements->frame_ns.data();
        buffer.capacity = measurements->frame_ns.size();
        observer = &record_periodic_frame;
        observer_data = &buffer;
    }

    const auto status = runtime.run_periodic(
        config,
        observer,
        observer_data,
        &result);
    if (!report_status("periodic run", status, runtime)) {
        return false;
    }
    if (measurements != nullptr) {
        if (buffer.count != frame_count ||
            result.frames_executed != frame_count) {
            std::cerr << "Periodic run returned an incomplete frame set\n";
            return false;
        }
        measurements->missed_frames = buffer.missed_frames;
        measurements->watchdog_trips = buffer.watchdog_trips;
    }
    return true;
}

double percentile(
    const std::vector<std::uint64_t>& sorted,
    std::size_t numerator) {
    const auto rank = (numerator * sorted.size() + 99) / 100;
    const auto index = rank == 0 ? 0 : rank - 1;
    return static_cast<double>(sorted[index]) / 1'000'000.0;
}

struct Summary {
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double mean_ms = 0.0;
    double stdev_ms = 0.0;
};

Summary summarize(std::vector<std::uint64_t>& frame_ns) {
    long double sum = 0.0L;
    for (const auto value : frame_ns) {
        sum += static_cast<long double>(value);
    }
    const auto count = static_cast<long double>(frame_ns.size());
    const long double mean = sum / count;
    long double squared_error = 0.0L;
    for (const auto value : frame_ns) {
        const auto delta = static_cast<long double>(value) - mean;
        squared_error += delta * delta;
    }

    std::sort(frame_ns.begin(), frame_ns.end());
    Summary summary;
    summary.p50_ms = percentile(frame_ns, 50);
    summary.p95_ms = percentile(frame_ns, 95);
    summary.p99_ms = percentile(frame_ns, 99);
    summary.mean_ms = static_cast<double>(mean / 1'000'000.0L);
    summary.stdev_ms =
        static_cast<double>(std::sqrt(squared_error / count) / 1'000'000.0L);
    return summary;
}

const char* executor_policy_name(rt::ExecutorPolicy policy) noexcept {
    switch (policy) {
    case rt::ExecutorPolicy::static_deterministic:
        return "static_deterministic";
    case rt::ExecutorPolicy::bounded_throughput:
        return "bounded_throughput";
    case rt::ExecutorPolicy::host_adapter:
        return "host_adapter";
    }
    return "unknown";
}

void print_json(
    const Options& options,
    const rt::RuntimeProfileMetadata& profile,
    const rt::ObservabilityMetadata& metadata,
    const rt::ExecutorStats& executor,
    const RunMeasurements& measurements,
    const Summary& summary,
    const BarrierState& barrier) {
    std::cout << std::fixed << std::setprecision(6)
              << '{'
              << "\"ok\":true,"
              << "\"profile_schema_version\":" << profile.schema_version << ','
              << "\"profile_id\":\"" << profile.profile_id.data() << "\","
              << "\"runtime_config_schema\":"
              << profile.runtime_config_schema << ','
              << "\"runtime_version\":\"" RTFW_VERSION_STRING "\","
              << "\"config_id\":" << metadata.config_id << ','
              << "\"seed\":" << options.seed << ','
              << "\"self_paced\":"
              << (options.self_paced ? "true" : "false") << ','
              << "\"worker_override\":"
              << (options.worker_override.has_value() ? "true" : "false")
              << ','
              << "\"frames\":" << measurements.frame_ns.size() << ','
              << "\"period_ns\":" << kFramePeriod.count() << ','
              << "\"p50_frame_ms\":" << summary.p50_ms << ','
              << "\"p95_frame_ms\":" << summary.p95_ms << ','
              << "\"p99_frame_ms\":" << summary.p99_ms << ','
              << "\"mean_frame_ms\":" << summary.mean_ms << ','
              << "\"stdev_frame_ms\":" << summary.stdev_ms << ','
              << "\"missed_frames\":" << measurements.missed_frames << ','
              << "\"watchdog_trips\":" << measurements.watchdog_trips << ','
              << "\"trace_events_dropped\":"
              << measurements.trace_events_dropped << ','
              // Compatibility alias for the existing autotune result schema.
              << "\"log_drops\":" << measurements.trace_events_dropped << ','
              << "\"queue_rejections\":"
              << executor.queue_full_rejections << ','
              << "\"checksum\":" << barrier.checksum << ','
              << "\"phases\":{\"frame\":{"
              << "\"p50_ms\":" << summary.p50_ms << ','
              << "\"p95_ms\":" << summary.p95_ms << ','
              << "\"p99_ms\":" << summary.p99_ms << "}},"
              << "\"counters\":{"
              << "\"missed_frames\":" << measurements.missed_frames << ','
              << "\"watchdog_trips\":" << measurements.watchdog_trips << ','
              << "\"trace.events_dropped\":"
              << measurements.trace_events_dropped << ','
              << "\"log_drops\":" << measurements.trace_events_dropped << ','
              << "\"executor.queue_rejections\":"
              << executor.queue_full_rejections << "},"
              << "\"executor\":{"
              << "\"policy\":\""
              << executor_policy_name(executor.policy) << "\","
              << "\"worker_count\":" << executor.worker_count << ','
              << "\"queue_capacity\":" << executor.queue_capacity << ','
              << "\"submitted_tasks\":" << executor.submitted_tasks << ','
              << "\"successful_steals\":"
              << executor.successful_steals << "}"
              << "}\n";
}

int run_demo(
    const Options& options,
    rt::RuntimeConfig config,
    const rt::RuntimeProfileMetadata& profile) {
    if (options.worker_override.has_value()) {
        config.worker_count = *options.worker_override;
    }
    if (config.executor_policy == rt::ExecutorPolicy::host_adapter) {
        std::cerr
            << "rtfw_runtime_demo cannot supply a borrowed host executor; "
            << "choose a native executor policy\n";
        return 2;
    }

    const auto warmup_frames = options.warmup.count() == 0
        ? std::size_t{0}
        : frame_count_for(options.warmup);
    const auto measured_frames = frame_count_for(options.run);
    if (measured_frames == 0 ||
        (options.warmup.count() != 0 && warmup_frames == 0)) {
        std::cerr << "Requested duration exceeds the demo frame limit\n";
        return 2;
    }

    std::array<BodySystem, kSystemCount> systems;
    for (std::size_t index = 0; index < systems.size(); ++index) {
        systems[index].initialize(kBodiesPerSystem, options.seed, index);
    }
    BarrierState barrier;
    RunMeasurements measurements;
    measurements.frame_ns.resize(measured_frames);

    rt::Runtime runtime;
    if (!report_status("configure runtime", runtime.configure(config), runtime) ||
        !build_workload(runtime, systems, barrier) ||
        !report_status("finalize runtime", runtime.finalize(), runtime) ||
        !report_status("start runtime", runtime.start(), runtime)) {
        return 2;
    }

    const auto run_frames = options.self_paced
        ? &run_periodic_frames
        : &run_host_frames;
    bool ok = true;
    if (warmup_frames != 0) {
        ok = run_frames(runtime, 0, warmup_frames, nullptr);
    }
    rt::RuntimeMetricCursor metric_cursor;
    rt::RuntimeMetricSnapshot baseline_metrics;
    if (ok) {
        ok = report_status(
            "start measured metric interval",
            runtime.metrics_snapshot(
                rt::RuntimeMetricWindow::interval,
                &metric_cursor,
                baseline_metrics),
            runtime);
    }
    if (ok) {
        ok = run_frames(
            runtime,
            static_cast<std::uint64_t>(warmup_frames),
            measured_frames,
            &measurements);
    }
    const auto stop_status = runtime.stop();
    if (!report_status("stop runtime", stop_status, runtime)) {
        ok = false;
    }
    if (!ok) {
        return 2;
    }

    rt::ObservabilityMetadata metadata;
    if (!report_status(
            "read observability metadata",
            runtime.observability_metadata(metadata),
            runtime)) {
        return 2;
    }
    const auto executor = runtime.executor_stats();
    rt::RuntimeMetricSnapshot runtime_metrics;
    if (!report_status(
            "read runtime metrics",
            runtime.metrics_snapshot(
                rt::RuntimeMetricWindow::interval,
                &metric_cursor,
                runtime_metrics),
            runtime)) {
        return 2;
    }
    const auto trace_drop_index = static_cast<std::size_t>(
        rt::RuntimeMetricId::trace_events_dropped);
    if (trace_drop_index >= runtime_metrics.sample_count ||
        runtime_metrics.samples[trace_drop_index].id !=
            rt::RuntimeMetricId::trace_events_dropped) {
        std::cerr << "Runtime metric schema is missing trace drop data\n";
        return 2;
    }
    measurements.trace_events_dropped =
        runtime_metrics.samples[trace_drop_index].value;
    const auto summary = summarize(measurements.frame_ns);

    if (options.metrics_json) {
        print_json(
            options,
            profile,
            metadata,
            executor,
            measurements,
            summary,
            barrier);
    } else {
        std::cout << "profile=" << profile.profile_id.data()
                  << " frames=" << measurements.frame_ns.size()
                  << " p99_ms=" << std::fixed << std::setprecision(6)
                  << summary.p99_ms
                  << " missed=" << measurements.missed_frames
                  << '\n';
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        return 2;
    }
    if (options.show_help) {
        print_help(argv[0]);
        return 0;
    }
    if (options.show_version) {
        std::cout << RTFW_VERSION_STRING << '\n';
        return 0;
    }
    if (options.profile_path.empty()) {
        std::cerr
            << "A runtime profile is required via --config or RTFW_PROFILE\n";
        return 2;
    }

    std::string profile_json;
    if (!read_profile_file(options.profile_path, profile_json)) {
        return 2;
    }
    rt::RuntimeConfig config;
    rt::RuntimeProfileMetadata metadata;
    rt::RuntimeProfileError error;
    const auto status = rt::parse_runtime_profile(
        profile_json,
        config,
        metadata,
        error);
    if (status != rt::Status::ok) {
        std::cerr << "Runtime profile rejected at byte "
                  << error.byte_offset << " (" << error.path.data() << "): "
                  << rt::runtime_profile_error_message(error.code) << '\n';
        return 2;
    }
    return run_demo(options, config, metadata);
}
