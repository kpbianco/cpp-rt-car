#pragma once

// Optional host-side, C++20 source API. No Runtime dependency or C ABI promise.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rtfw::benchmark {
inline constexpr std::uint32_t schema_version = 1;
inline constexpr std::size_t max_providers = 32, max_cases = 256;
inline constexpr std::size_t max_parameters = 16, max_counters = 16;
inline constexpr std::uint32_t max_warmup = 1000, max_repetitions = 10000;
inline constexpr std::uint64_t max_integer = 0x7fffffffffffffffULL;

enum class Status {
    ok, invalid, duplicate, capacity, not_found, stale, busy,
    provider_error, clock_error, invariant_failed, not_run, io_error, exists
};
enum class ClockKind { fake, steady };
[[nodiscard]] const char* status_name(Status) noexcept;

struct Parameter {
    std::string name;
    std::uint64_t value{}, minimum{}, maximum{};
};
struct Counter {
    std::string name;
    std::string unit{"count"};
    std::uint64_t minimum{}, maximum{max_integer};
};
struct Descriptor {
    std::uint32_t version{schema_version};
    std::string case_id, subsystem, implementation, configuration;
    std::string workload_kind, workload_sha256;
    std::vector<Parameter> parameters;
    std::vector<Counter> counters;
    std::uint32_t warmup{2}, repetitions{5};
    // Version 1 retains every measured sample. No implicit sampling/truncation.
    bool retain_raw{true};
};
struct Observation {
    std::vector<std::uint64_t> counters;
    // Application correctness token, not a pointer or proprietary payload.
    std::uint64_t checksum{};
    bool correct{true};
};
struct ProviderV1 {
    std::uint32_t size{sizeof(ProviderV1)}, version{schema_version};
    const char* id{};
    std::uint32_t implementation_version{1};
    std::size_t case_count{};
    void* user{};
    Status (*describe)(void*, std::size_t, Descriptor&){};
    Status (*invoke)(void*, std::string_view, std::uint64_t, Observation&){};
    std::uint64_t reserved[2]{};
};
struct ProviderHandle {
    const void* owner{};
    std::uint64_t generation{};
};
struct ClockV1 {
    std::uint32_t size{sizeof(ClockV1)}, version{schema_version};
    ClockKind kind{ClockKind::steady};
    void* user{};
    bool (*read_ns)(void*, std::uint64_t&){};
};
[[nodiscard]] ClockV1 steady_clock() noexcept;

// Optional observations are explicit. These fields contain sanitized labels,
// never absolute paths, environment values, machine IDs, or authentication.
struct Identity {
    std::string source_commit{"unknown"}, source_tree{"unknown"};
    std::string source_dirty{"unknown"};  // "true", "false", or "unknown"
    std::string compiler{"unknown"}, compiler_version{"unknown"};
    std::string build_configuration{"unknown"}, build_flags_sha256{"unknown"};
    std::string os{"unknown"}, kernel{"unknown"}, architecture{"unknown"};
    std::string cpu_model{"not_available"}, host_label{"not_available"};
    std::uint64_t logical_cpus{}, total_memory_bytes{}, page_size{};
    // Zero above is serialized as "not_available", never as a measurement.
    std::string thread_policy{"not_available"}, memory_policy{"not_available"};
    std::string backend{"not_available"}, driver{"not_available"};
};
[[nodiscard]] Identity capture_identity();
[[nodiscard]] Status validate(const Descriptor&) noexcept;
[[nodiscard]] Status validate(const Identity&) noexcept;
[[nodiscard]] std::string sha256(std::string_view);

struct Sample {
    std::uint64_t index{}, start_ns{}, end_ns{};
    Observation observation;
    bool invariants_passed{};
};
struct Result {
    Descriptor descriptor;
    std::string provider_id;
    std::uint32_t provider_version{};
    Identity identity;
    ClockKind clock{ClockKind::steady};
    std::string start_utc, end_utc;
    std::uint32_t warmup_completed{};
    std::vector<Sample> samples;
    Status status{Status::invalid};
    std::string diagnostic;
};
struct Artifacts {
    std::string descriptor, raw, summary;
};

class Runner {
public:
    Runner() = default;
    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;
    Runner(Runner&&) = delete;
    Runner& operator=(Runner&&) = delete;
    // The table/descriptor values are copied. user and callable lifetimes are
    // borrowed until unregister/destruction. Calls on one Runner are serialized
    // by its owner; independent instances share no registry or mutable state.
    [[nodiscard]] Status register_provider(const ProviderV1&, ProviderHandle&);
    [[nodiscard]] Status unregister_provider(ProviderHandle) noexcept;
    [[nodiscard]] std::vector<std::string> list() const;
    [[nodiscard]] Status describe(std::string_view provider, std::string_view id,
                                  Descriptor&) const;
    [[nodiscard]] Result run(std::string_view provider, std::string_view id,
                             const ClockV1&, const Identity&);
private:
    struct Registered {
        ProviderV1 table;
        std::string id;
        std::vector<Descriptor> cases;
        std::uint64_t generation;
    };
    std::vector<Registered> providers_;
    std::uint64_t next_generation_{1};
    bool active_{};
};

// Canonical integer-only JSON. Failure results remain failure results; no
// summary is manufactured from missing observations. SHA-256 is not signing.
[[nodiscard]] std::string encode_descriptor(std::string_view provider, std::uint32_t provider_version,
                                            const Descriptor&, ClockKind);
[[nodiscard]] Artifacts encode(const Result&);
// The output directory must not exist; its parents must already exist and may
// not be symlinks/reparse points. Publishes exactly three files atomically.
// No overwrite and no fallback to a racy check-then-rename implementation.
[[nodiscard]] Status check_destination(const std::filesystem::path&) noexcept;
[[nodiscard]] Status publish(const Result&, const std::filesystem::path&);
}  // namespace rtfw::benchmark
