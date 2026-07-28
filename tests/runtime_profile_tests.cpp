#include <rt/profile.hpp>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <new>
#include <string>
#include <string_view>

namespace {

std::atomic<bool> track_allocations{false};
std::atomic<std::size_t> allocation_count{0};

constexpr const char* valid_profile = R"json({
  "schema_version": 1,
  "profile_id": "tests.profile",
  "runtime_compatibility": {
    "major": 1,
    "minimum_minor": 1
  },
  "runtime_config_schema": 7,
  "runtime": {
    "callback_capacity": 4,
    "scratch_bytes": 256,
    "trace_capacity": 64,
    "numerical_mode": "precise",
    "executor_policy": "bounded_throughput",
    "worker_count": 2,
    "executor_queue_capacity": 128,
    "scratch_alignment": 64,
    "task_scratch_bytes": 128,
    "task_scratch_slots": 32,
    "memory_budget_bytes": 1048576,
    "overload_policy": "fail_frame",
    "watchdog_timeout_ns": 0,
    "watchdog_max_degradation_level": 0,
    "platform_preflight_mode": "disabled",
    "determinism_tier": "d0",
    "state_capacity": 8,
    "snapshot_max_bytes": 4096,
    "replay_input_capacity": 16,
    "input_log_max_bytes": 4096,
    "device_backend_capacity": 1,
    "device_buffer_capacity": 8,
    "device_outstanding_capacity": 4,
    "device_completion_batch": 2,
    "workload_id": "tests.profile"
  },
  "params": {
    "worker_count": 2,
    "nested": [true, null, {"value": -1.25e2}]
  }
})json";

void require(bool condition) {
    if (!condition) {
        std::fprintf(
            stderr,
            "runtime_profile_tests assertion failed\n");
        std::abort();
    }
}

std::string replace_once(
    std::string source,
    const std::string& before,
    const std::string& after) {
    const auto offset = source.find(before);
    require(offset != std::string::npos);
    source.replace(offset, before.size(), after);
    return source;
}

void valid_profile_is_transactional_and_allocation_free() {
    rt::RuntimeConfig config;
    config.worker_count = 7;
    rt::RuntimeProfileMetadata metadata;
    rt::RuntimeProfileError error;

    allocation_count.store(0, std::memory_order_relaxed);
    track_allocations.store(true, std::memory_order_release);
    const auto status =
        rt::parse_runtime_profile(valid_profile, config, metadata, error);
    track_allocations.store(false, std::memory_order_release);

    if (status != rt::Status::ok) {
        std::fprintf(
            stderr,
            "profile parse failed: %s at %s byte %zu\n",
            rt::runtime_profile_error_message(error.code),
            error.path.data(),
            error.byte_offset);
    }
    require(status == rt::Status::ok);
    require(allocation_count.load(std::memory_order_relaxed) == 0);
    require(error.code == rt::RuntimeProfileErrorCode::none);
    require(config.callback_capacity == 4);
    require(config.worker_count == 2);
    require(config.executor_policy == rt::ExecutorPolicy::bounded_throughput);
    require(config.device_outstanding_capacity == 4);
    require(config.device_completion_batch == 2);
    require(metadata.schema_version == 1);
    require(metadata.runtime_config_schema == 7);
    require(metadata.runtime_major == 1);
    require(metadata.minimum_runtime_minor == 1);
    require(std::string(metadata.profile_id.data()) == "tests.profile");
}

void malformed_profiles_fail_without_mutation() {
    const std::string original(valid_profile);
    const std::string duplicate = replace_once(
        original,
        "\"worker_count\": 2,",
        "\"worker_count\": 2, \"worker_count\": 4,");
    const std::string unknown = replace_once(
        original,
        "\"worker_count\": 2,",
        "\"worker_count\": 2, \"worker_magic\": 4,");
    const std::string missing = replace_once(
        original,
        "\"worker_count\": 2,",
        "");
    const std::string incompatible = replace_once(
        original,
        "\"minimum_minor\": 1",
        "\"minimum_minor\": 999");
    const std::string invalid_pair = replace_once(
        original,
        "\"device_completion_batch\": 2",
        "\"device_completion_batch\": 8");
    const std::string invalid_type = replace_once(
        original,
        "\"worker_count\": 2",
        "\"worker_count\": \"2\"");
    const std::string invalid_queue = replace_once(
        original,
        "\"executor_queue_capacity\": 128",
        "\"executor_queue_capacity\": 127");
    const std::string params_not_object = replace_once(
        original,
        "\"params\": {",
        "\"params\": [");
    const std::string top_level_duplicate = replace_once(
        original,
        "\"schema_version\": 1,",
        "\"schema_version\": 1, \"schema_version\": 1,");

    struct Case {
        const std::string* profile;
        rt::Status status;
        rt::RuntimeProfileErrorCode code;
    };
    const Case cases[] = {
        {&duplicate, rt::Status::invalid_config,
         rt::RuntimeProfileErrorCode::duplicate_key},
        {&unknown, rt::Status::invalid_config,
         rt::RuntimeProfileErrorCode::unknown_key},
        {&missing, rt::Status::invalid_config,
         rt::RuntimeProfileErrorCode::missing_key},
        {&incompatible, rt::Status::incompatible_artifact,
         rt::RuntimeProfileErrorCode::incompatible_runtime},
        {&invalid_pair, rt::Status::invalid_config,
         rt::RuntimeProfileErrorCode::invalid_value},
        {&invalid_type, rt::Status::invalid_config,
         rt::RuntimeProfileErrorCode::invalid_type},
        {&invalid_queue, rt::Status::invalid_config,
         rt::RuntimeProfileErrorCode::invalid_value},
        {&params_not_object, rt::Status::invalid_config,
         rt::RuntimeProfileErrorCode::invalid_type},
        {&top_level_duplicate, rt::Status::invalid_config,
         rt::RuntimeProfileErrorCode::duplicate_key},
    };

    for (const auto& test : cases) {
        rt::RuntimeConfig config;
        config.worker_count = 7;
        rt::RuntimeProfileMetadata metadata;
        metadata.schema_version = 42;
        rt::RuntimeProfileError error;
        require(
            rt::parse_runtime_profile(
                *test.profile,
                config,
                metadata,
                error) == test.status);
        require(error.code == test.code);
        require(config.worker_count == 7);
        require(metadata.schema_version == 42);
    }
}

void compatibility_and_diagnostics_are_explicit() {
    const std::string older_minor = replace_once(
        valid_profile,
        "\"minimum_minor\": 1",
        "\"minimum_minor\": 0");
    rt::RuntimeConfig config;
    rt::RuntimeProfileMetadata metadata;
    rt::RuntimeProfileError error;
    require(
        rt::parse_runtime_profile(
            older_minor,
            config,
            metadata,
            error) == rt::Status::ok);
    require(metadata.minimum_runtime_minor == 0);

    const std::string invalid_value = replace_once(
        valid_profile,
        "\"device_completion_batch\": 2",
        "\"device_completion_batch\": 8");
    require(
        rt::parse_runtime_profile(
            invalid_value,
            config,
            metadata,
            error) == rt::Status::invalid_config);
    require(
        std::string_view(error.path.data()) ==
        "$.runtime.device_completion_batch");

    std::string invalid_utf8(valid_profile);
    const auto params_offset = invalid_utf8.find("\"nested\"");
    require(params_offset != std::string::npos);
    invalid_utf8[params_offset] = static_cast<char>(0xff);
    require(
        rt::parse_runtime_profile(
            invalid_utf8,
            config,
            metadata,
            error) == rt::Status::invalid_config);
    require(error.code == rt::RuntimeProfileErrorCode::syntax);
    require(error.byte_offset == params_offset);
}

void oversized_and_trailing_inputs_fail_closed() {
    std::string oversized(rt::runtime_profile_max_bytes + 1, ' ');
    rt::RuntimeConfig config;
    rt::RuntimeProfileMetadata metadata;
    rt::RuntimeProfileError error;
    require(
        rt::parse_runtime_profile(
            oversized,
            config,
            metadata,
            error) == rt::Status::invalid_config);
    require(
        error.code == rt::RuntimeProfileErrorCode::input_too_large);

    const std::string trailing = std::string(valid_profile) + "{}";
    require(
        rt::parse_runtime_profile(
            trailing,
            config,
            metadata,
            error) == rt::Status::invalid_config);
    require(error.code == rt::RuntimeProfileErrorCode::syntax);
}

void bounded_params_and_runtime_boundaries_are_enforced() {
    const std::string minimum_input_log = replace_once(
        valid_profile,
        "\"input_log_max_bytes\": 4096",
        "\"input_log_max_bytes\": 192");
    rt::RuntimeConfig config;
    rt::RuntimeProfileMetadata metadata;
    rt::RuntimeProfileError error;
    require(
        rt::parse_runtime_profile(
            minimum_input_log,
            config,
            metadata,
            error) == rt::Status::ok);
    require(config.input_log_max_bytes == 192);

    std::string excessive_nesting(valid_profile);
    const auto params_offset = excessive_nesting.find("\"params\"");
    const auto root_close = excessive_nesting.rfind('}');
    require(params_offset != std::string::npos);
    require(root_close != std::string::npos);
    std::string replacement = "\"params\":{\"nested\":";
    replacement.append(17, '[');
    replacement += '0';
    replacement.append(17, ']');
    replacement += '}';
    excessive_nesting.replace(
        params_offset,
        root_close - params_offset,
        replacement);

    config.worker_count = 7;
    metadata.schema_version = 42;
    require(
        rt::parse_runtime_profile(
            excessive_nesting,
            config,
            metadata,
            error) == rt::Status::invalid_config);
    require(error.code == rt::RuntimeProfileErrorCode::invalid_value);
    require(std::string_view(error.path.data()) == "$.params");
    require(config.worker_count == 7);
    require(metadata.schema_version == 42);
}

void mutation_corpus_stays_bounded_and_transactional() {
    for (std::size_t iteration = 0; iteration < 2048; ++iteration) {
        std::string mutated(valid_profile);
        const auto offset = (iteration * 131u) % mutated.size();
        const auto mask =
            static_cast<unsigned char>(1u << (iteration % 7u));
        mutated[offset] = static_cast<char>(
            static_cast<unsigned char>(mutated[offset]) ^ mask);

        rt::RuntimeConfig config;
        config.worker_count = 7;
        rt::RuntimeProfileMetadata metadata;
        metadata.schema_version = 42;
        rt::RuntimeProfileError error;

        allocation_count.store(0, std::memory_order_relaxed);
        track_allocations.store(true, std::memory_order_release);
        const auto status = rt::parse_runtime_profile(
            mutated,
            config,
            metadata,
            error);
        track_allocations.store(false, std::memory_order_release);
        require(allocation_count.load(std::memory_order_relaxed) == 0);

        if (status == rt::Status::ok) {
            require(error.code == rt::RuntimeProfileErrorCode::none);
        } else {
            require(config.worker_count == 7);
            require(metadata.schema_version == 42);
            require(error.code != rt::RuntimeProfileErrorCode::none);
        }
    }
}

} // namespace

void* operator new(std::size_t size) {
    if (track_allocations.load(std::memory_order_relaxed)) {
        allocation_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* storage = std::malloc(size == 0 ? 1 : size)) {
        return storage;
    }
    throw std::bad_alloc();
}

void operator delete(void* storage) noexcept {
    std::free(storage);
}

void operator delete(void* storage, std::size_t) noexcept {
    std::free(storage);
}

int main() {
    valid_profile_is_transactional_and_allocation_free();
    malformed_profiles_fail_without_mutation();
    compatibility_and_diagnostics_are_explicit();
    oversized_and_trailing_inputs_fail_closed();
    bounded_params_and_runtime_boundaries_are_enforced();
    mutation_corpus_stays_bounded_and_transactional();
    return 0;
}
