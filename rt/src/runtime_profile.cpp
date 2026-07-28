#include "rt/profile.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "rtfw/version.h"

namespace {

enum class ValueKind : std::uint8_t {
    unsigned_integer,
    string,
};

struct ConfigField {
    std::string_view name;
    ValueKind kind;
};

constexpr std::array<ConfigField, 25> kConfigFields{{
    {"callback_capacity", ValueKind::unsigned_integer},
    {"scratch_bytes", ValueKind::unsigned_integer},
    {"trace_capacity", ValueKind::unsigned_integer},
    {"numerical_mode", ValueKind::string},
    {"executor_policy", ValueKind::string},
    {"worker_count", ValueKind::unsigned_integer},
    {"executor_queue_capacity", ValueKind::unsigned_integer},
    {"scratch_alignment", ValueKind::unsigned_integer},
    {"task_scratch_bytes", ValueKind::unsigned_integer},
    {"task_scratch_slots", ValueKind::unsigned_integer},
    {"memory_budget_bytes", ValueKind::unsigned_integer},
    {"overload_policy", ValueKind::string},
    {"watchdog_timeout_ns", ValueKind::unsigned_integer},
    {"watchdog_max_degradation_level", ValueKind::unsigned_integer},
    {"platform_preflight_mode", ValueKind::string},
    {"determinism_tier", ValueKind::string},
    {"state_capacity", ValueKind::unsigned_integer},
    {"snapshot_max_bytes", ValueKind::unsigned_integer},
    {"replay_input_capacity", ValueKind::unsigned_integer},
    {"input_log_max_bytes", ValueKind::unsigned_integer},
    {"device_backend_capacity", ValueKind::unsigned_integer},
    {"device_buffer_capacity", ValueKind::unsigned_integer},
    {"device_outstanding_capacity", ValueKind::unsigned_integer},
    {"device_completion_batch", ValueKind::unsigned_integer},
    {"workload_id", ValueKind::string},
}};

constexpr std::size_t kValueCapacity =
    rt::observability_identifier_capacity;
constexpr std::size_t kMaximumNesting = 16;

bool valid_utf8(
    std::string_view input,
    std::size_t& invalid_offset) noexcept {
    const auto continuation = [](unsigned char value) noexcept {
        return value >= 0x80u && value <= 0xbfu;
    };
    std::size_t index = 0;
    while (index < input.size()) {
        const auto first =
            static_cast<unsigned char>(input[index]);
        if (first <= 0x7fu) {
            ++index;
            continue;
        }

        std::size_t length = 0;
        if (first >= 0xc2u && first <= 0xdfu) {
            length = 2;
        } else if (first >= 0xe0u && first <= 0xefu) {
            length = 3;
        } else if (first >= 0xf0u && first <= 0xf4u) {
            length = 4;
        } else {
            invalid_offset = index;
            return false;
        }
        if (index + length > input.size()) {
            invalid_offset = index;
            return false;
        }

        const auto second =
            static_cast<unsigned char>(input[index + 1]);
        if (!continuation(second) ||
            (first == 0xe0u && second < 0xa0u) ||
            (first == 0xedu && second > 0x9fu) ||
            (first == 0xf0u && second < 0x90u) ||
            (first == 0xf4u && second > 0x8fu)) {
            invalid_offset = index;
            return false;
        }
        for (std::size_t offset = 2; offset < length; ++offset) {
            if (!continuation(
                    static_cast<unsigned char>(input[index + offset]))) {
                invalid_offset = index;
                return false;
            }
        }
        index += length;
    }
    return true;
}

struct ParsedValue {
    bool present = false;
    std::array<char, kValueCapacity> text{};
};

bool identifier_character(char value) noexcept {
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') ||
           value == '.' || value == '_' || value == ':' ||
           value == '/' || value == '@' || value == '-';
}

template <std::size_t Capacity>
bool valid_identifier(const std::array<char, Capacity>& value) noexcept {
    std::size_t index = 0;
    while (index < Capacity && value[index] != '\0') {
        if (!identifier_character(value[index])) {
            return false;
        }
        ++index;
    }
    return index != 0 && index < Capacity;
}

class Parser {
public:
    Parser(std::string_view input, rt::RuntimeProfileError& error) noexcept
        : input_(input), error_(error) {}

    bool parse(
        rt::RuntimeConfig& config,
        rt::RuntimeProfileMetadata& metadata) noexcept {
        std::array<ParsedValue, kConfigFields.size()> values{};
        bool saw_schema = false;
        bool saw_id = false;
        bool saw_compatibility = false;
        bool saw_config_schema = false;
        bool saw_runtime = false;
        bool saw_params = false;
        std::uint64_t schema = 0;
        std::uint64_t config_schema = 0;
        std::uint64_t runtime_major = 0;
        std::uint64_t minimum_minor = 0;
        std::array<char, rt::runtime_profile_identifier_capacity> profile_id{};

        if (!expect('{', "$")) {
            return false;
        }
        skip_space();
        if (take('}')) {
            return fail(
                rt::RuntimeProfileErrorCode::missing_key,
                "$.schema_version");
        }

        while (true) {
            std::array<char, kValueCapacity> key{};
            if (!parse_string(key, "$")) {
                return false;
            }
            if (!expect(':', "$")) {
                return false;
            }
            const std::string_view key_view(key.data());
            if (key_view == "schema_version") {
                if (saw_schema) {
                    return fail(
                        rt::RuntimeProfileErrorCode::duplicate_key,
                        "$.schema_version");
                }
                saw_schema = true;
                if (!parse_unsigned(schema, "$.schema_version")) {
                    return false;
                }
            } else if (key_view == "profile_id") {
                if (saw_id) {
                    return fail(
                        rt::RuntimeProfileErrorCode::duplicate_key,
                        "$.profile_id");
                }
                saw_id = true;
                if (!parse_string(profile_id, "$.profile_id")) {
                    return false;
                }
                if (!valid_identifier(profile_id)) {
                    return fail(
                        rt::RuntimeProfileErrorCode::invalid_value,
                        "$.profile_id");
                }
            } else if (key_view == "runtime_compatibility") {
                if (saw_compatibility) {
                    return fail(
                        rt::RuntimeProfileErrorCode::duplicate_key,
                        "$.runtime_compatibility");
                }
                saw_compatibility = true;
                if (!parse_compatibility(runtime_major, minimum_minor)) {
                    return false;
                }
            } else if (key_view == "runtime_config_schema") {
                if (saw_config_schema) {
                    return fail(
                        rt::RuntimeProfileErrorCode::duplicate_key,
                        "$.runtime_config_schema");
                }
                saw_config_schema = true;
                if (!parse_unsigned(
                        config_schema,
                        "$.runtime_config_schema")) {
                    return false;
                }
            } else if (key_view == "runtime") {
                if (saw_runtime) {
                    return fail(
                        rt::RuntimeProfileErrorCode::duplicate_key,
                        "$.runtime");
                }
                saw_runtime = true;
                if (!parse_runtime(values)) {
                    return false;
                }
            } else if (key_view == "params") {
                if (saw_params) {
                    return fail(
                        rt::RuntimeProfileErrorCode::duplicate_key,
                        "$.params");
                }
                saw_params = true;
                skip_space();
                if (position_ >= input_.size() ||
                    input_[position_] != '{') {
                    return fail(
                        rt::RuntimeProfileErrorCode::invalid_type,
                        "$.params");
                }
                if (!skip_value("$.params", 0)) {
                    return false;
                }
            } else {
                return fail(
                    rt::RuntimeProfileErrorCode::unknown_key,
                    "$");
            }

            skip_space();
            if (take('}')) {
                break;
            }
            if (!expect(',', "$")) {
                return false;
            }
        }

        skip_space();
        if (position_ != input_.size()) {
            return fail(rt::RuntimeProfileErrorCode::syntax, "$");
        }
        if (!saw_schema) {
            return fail(
                rt::RuntimeProfileErrorCode::missing_key,
                "$.schema_version");
        }
        if (!saw_id) {
            return fail(
                rt::RuntimeProfileErrorCode::missing_key,
                "$.profile_id");
        }
        if (!saw_compatibility) {
            return fail(
                rt::RuntimeProfileErrorCode::missing_key,
                "$.runtime_compatibility");
        }
        if (!saw_config_schema) {
            return fail(
                rt::RuntimeProfileErrorCode::missing_key,
                "$.runtime_config_schema");
        }
        if (!saw_runtime) {
            return fail(
                rt::RuntimeProfileErrorCode::missing_key,
                "$.runtime");
        }
        if (schema != rt::runtime_profile_schema_version ||
            config_schema != rt::runtime_config_schema_version) {
            return fail(
                rt::RuntimeProfileErrorCode::incompatible_schema,
                schema != rt::runtime_profile_schema_version
                    ? "$.schema_version"
                    : "$.runtime_config_schema");
        }
        if (runtime_major != RTFW_VERSION_MAJOR ||
            minimum_minor > RTFW_VERSION_MINOR) {
            return fail(
                rt::RuntimeProfileErrorCode::incompatible_runtime,
                "$.runtime_compatibility");
        }

        rt::RuntimeConfig candidate{};
        // Avoid an invalid transient when a profile reduces outstanding
        // capacity below the default completion batch.
        if (rt::set_runtime_config_value(
                candidate,
                "device_completion_batch",
                "1") != rt::Status::ok) {
            return fail(
                rt::RuntimeProfileErrorCode::invalid_value,
                "$.runtime.device_completion_batch");
        }
        for (std::size_t index = 0; index < kConfigFields.size(); ++index) {
            if (!values[index].present) {
                return fail_runtime_field(
                    rt::RuntimeProfileErrorCode::missing_key,
                    index);
            }
            // Determinism is applied last after its executor/watchdog
            // prerequisites. The final completion batch is also applied last
            // after outstanding capacity.
            if (kConfigFields[index].name == "determinism_tier" ||
                kConfigFields[index].name == "device_completion_batch") {
                continue;
            }
            if (rt::set_runtime_config_value(
                    candidate,
                    kConfigFields[index].name,
                    values[index].text.data()) != rt::Status::ok) {
                return fail_runtime_field(
                    rt::RuntimeProfileErrorCode::invalid_value,
                    index);
            }
        }
        for (const auto deferred : {
                 std::string_view("device_completion_batch"),
                 std::string_view("determinism_tier")}) {
            const auto index = field_index(deferred);
            if (index == kConfigFields.size() ||
                rt::set_runtime_config_value(
                    candidate,
                    deferred,
                    values[index].text.data()) != rt::Status::ok) {
                return fail_runtime_field(
                    rt::RuntimeProfileErrorCode::invalid_value,
                    index);
            }
        }

        rt::RuntimeProfileMetadata parsed_metadata{};
        parsed_metadata.schema_version = static_cast<std::uint32_t>(schema);
        parsed_metadata.runtime_config_schema =
            static_cast<std::uint32_t>(config_schema);
        parsed_metadata.runtime_major =
            static_cast<std::uint32_t>(runtime_major);
        parsed_metadata.minimum_runtime_minor =
            static_cast<std::uint32_t>(minimum_minor);
        parsed_metadata.profile_id = profile_id;
        config = candidate;
        metadata = parsed_metadata;
        return true;
    }

private:
    bool parse_compatibility(
        std::uint64_t& major,
        std::uint64_t& minimum_minor) noexcept {
        if (!expect('{', "$.runtime_compatibility")) {
            return false;
        }
        bool saw_major = false;
        bool saw_minor = false;
        skip_space();
        if (take('}')) {
            return fail(
                rt::RuntimeProfileErrorCode::missing_key,
                "$.runtime_compatibility.major");
        }
        while (true) {
            std::array<char, kValueCapacity> key{};
            if (!parse_string(key, "$.runtime_compatibility") ||
                !expect(':', "$.runtime_compatibility")) {
                return false;
            }
            const std::string_view key_view(key.data());
            if (key_view == "major") {
                if (saw_major) {
                    return fail(
                        rt::RuntimeProfileErrorCode::duplicate_key,
                        "$.runtime_compatibility.major");
                }
                saw_major = true;
                if (!parse_unsigned(
                        major,
                        "$.runtime_compatibility.major")) {
                    return false;
                }
            } else if (key_view == "minimum_minor") {
                if (saw_minor) {
                    return fail(
                        rt::RuntimeProfileErrorCode::duplicate_key,
                        "$.runtime_compatibility.minimum_minor");
                }
                saw_minor = true;
                if (!parse_unsigned(
                        minimum_minor,
                        "$.runtime_compatibility.minimum_minor")) {
                    return false;
                }
            } else {
                return fail(
                    rt::RuntimeProfileErrorCode::unknown_key,
                    "$.runtime_compatibility");
            }
            skip_space();
            if (take('}')) {
                break;
            }
            if (!expect(',', "$.runtime_compatibility")) {
                return false;
            }
        }
        if (!saw_major || !saw_minor) {
            return fail(
                rt::RuntimeProfileErrorCode::missing_key,
                "$.runtime_compatibility");
        }
        return true;
    }

    bool parse_runtime(
        std::array<ParsedValue, kConfigFields.size()>& values) noexcept {
        if (!expect('{', "$.runtime")) {
            return false;
        }
        skip_space();
        if (take('}')) {
            return fail(
                rt::RuntimeProfileErrorCode::missing_key,
                "$.runtime");
        }
        while (true) {
            std::array<char, kValueCapacity> key{};
            if (!parse_string(key, "$.runtime") ||
                !expect(':', "$.runtime")) {
                return false;
            }
            const auto index = field_index(key.data());
            if (index == kConfigFields.size()) {
                return fail_runtime_key(
                    rt::RuntimeProfileErrorCode::unknown_key,
                    key.data());
            }
            if (values[index].present) {
                return fail_runtime_field(
                    rt::RuntimeProfileErrorCode::duplicate_key,
                    index);
            }
            values[index].present = true;
            std::array<char, rt::runtime_profile_error_path_capacity>
                value_path{};
            const auto path = runtime_field_path(index, value_path);
            if (kConfigFields[index].kind == ValueKind::string) {
                if (!parse_string(values[index].text, path)) {
                    return false;
                }
            } else if (!parse_unsigned_text(
                           values[index].text,
                           path)) {
                return false;
            }
            skip_space();
            if (take('}')) {
                break;
            }
            if (!expect(',', "$.runtime")) {
                return false;
            }
        }
        return true;
    }

    std::size_t field_index(std::string_view key) const noexcept {
        for (std::size_t index = 0; index < kConfigFields.size(); ++index) {
            if (kConfigFields[index].name == key) {
                return index;
            }
        }
        return kConfigFields.size();
    }

    std::string_view runtime_field_path(
        std::size_t index,
        std::array<
            char,
            rt::runtime_profile_error_path_capacity>& output) const noexcept {
        constexpr std::string_view prefix = "$.runtime.";
        std::size_t count = 0;
        for (const char character : prefix) {
            output[count++] = character;
        }
        if (index < kConfigFields.size()) {
            for (const char character : kConfigFields[index].name) {
                if (count + 1 >= output.size()) {
                    break;
                }
                output[count++] = character;
            }
        }
        output[count] = '\0';
        return {output.data(), count};
    }

    bool fail_runtime_key(
        rt::RuntimeProfileErrorCode code,
        std::string_view key) noexcept {
        std::array<char, rt::runtime_profile_error_path_capacity> path{};
        constexpr std::string_view prefix = "$.runtime.";
        std::size_t count = 0;
        for (const char character : prefix) {
            path[count++] = character;
        }
        for (const char character : key) {
            if (count + 1 >= path.size()) {
                break;
            }
            path[count++] = character;
        }
        path[count] = '\0';
        return fail(code, {path.data(), count});
    }

    bool fail_runtime_field(
        rt::RuntimeProfileErrorCode code,
        std::size_t index) noexcept {
        if (index >= kConfigFields.size()) {
            return fail(code, "$.runtime");
        }
        std::array<char, rt::runtime_profile_error_path_capacity> path{};
        return fail(code, runtime_field_path(index, path));
    }

    bool parse_unsigned(
        std::uint64_t& value,
        std::string_view path) noexcept {
        std::array<char, kValueCapacity> text{};
        if (!parse_unsigned_text(text, path)) {
            return false;
        }
        const auto* begin = text.data();
        const auto* end = begin;
        while (*end != '\0') {
            ++end;
        }
        const auto result = std::from_chars(begin, end, value, 10);
        if (result.ec != std::errc{} || result.ptr != end) {
            return fail(rt::RuntimeProfileErrorCode::invalid_value, path);
        }
        return true;
    }

    template <std::size_t Capacity>
    bool parse_unsigned_text(
        std::array<char, Capacity>& output,
        std::string_view path) noexcept {
        skip_space();
        const auto start = position_;
        if (position_ >= input_.size() ||
            input_[position_] < '0' ||
            input_[position_] > '9') {
            return fail(rt::RuntimeProfileErrorCode::invalid_type, path);
        }
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() &&
                input_[position_] >= '0' &&
                input_[position_] <= '9') {
                return fail(
                    rt::RuntimeProfileErrorCode::invalid_value,
                    path);
            }
        } else {
            while (position_ < input_.size() &&
                   input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
        }
        const auto length = position_ - start;
        if (length == 0 || length >= Capacity) {
            return fail(rt::RuntimeProfileErrorCode::invalid_value, path);
        }
        for (std::size_t index = 0; index < length; ++index) {
            output[index] = input_[start + index];
        }
        output[length] = '\0';
        return true;
    }

    template <std::size_t Capacity>
    bool parse_string(
        std::array<char, Capacity>& output,
        std::string_view path) noexcept {
        skip_space();
        if (!take('"')) {
            return fail(rt::RuntimeProfileErrorCode::invalid_type, path);
        }
        std::size_t output_size = 0;
        while (position_ < input_.size()) {
            char value = input_[position_++];
            if (value == '"') {
                if (output_size >= Capacity) {
                    return fail(
                        rt::RuntimeProfileErrorCode::invalid_value,
                        path);
                }
                output[output_size] = '\0';
                return true;
            }
            if (static_cast<unsigned char>(value) < 0x20u) {
                return fail(rt::RuntimeProfileErrorCode::syntax, path);
            }
            if (value == '\\') {
                if (position_ >= input_.size()) {
                    return fail(rt::RuntimeProfileErrorCode::syntax, path);
                }
                const char escape = input_[position_++];
                switch (escape) {
                case '"':
                case '\\':
                case '/':
                    value = escape;
                    break;
                case 'b':
                    value = '\b';
                    break;
                case 'f':
                    value = '\f';
                    break;
                case 'n':
                    value = '\n';
                    break;
                case 'r':
                    value = '\r';
                    break;
                case 't':
                    value = '\t';
                    break;
                default:
                    // Runtime keys and identifiers are ASCII. Reject Unicode
                    // escapes rather than applying an incomplete decoder.
                    return fail(rt::RuntimeProfileErrorCode::syntax, path);
                }
            }
            if (output_size + 1 >= Capacity) {
                return fail(
                    rt::RuntimeProfileErrorCode::invalid_value,
                    path);
            }
            output[output_size++] = value;
        }
        return fail(rt::RuntimeProfileErrorCode::syntax, path);
    }

    bool skip_value(
        std::string_view path,
        std::size_t depth) noexcept {
        if (depth > kMaximumNesting) {
            return fail(rt::RuntimeProfileErrorCode::invalid_value, path);
        }
        skip_space();
        if (position_ >= input_.size()) {
            return fail(rt::RuntimeProfileErrorCode::syntax, path);
        }
        const char value = input_[position_];
        if (value == '"') {
            return skip_string(path);
        }
        if (value == '{') {
            ++position_;
            skip_space();
            if (take('}')) {
                return true;
            }
            while (true) {
                if (!skip_string(path) || !expect(':', path) ||
                    !skip_value(path, depth + 1)) {
                    return false;
                }
                skip_space();
                if (take('}')) {
                    return true;
                }
                if (!expect(',', path)) {
                    return false;
                }
            }
        }
        if (value == '[') {
            ++position_;
            skip_space();
            if (take(']')) {
                return true;
            }
            while (true) {
                if (!skip_value(path, depth + 1)) {
                    return false;
                }
                skip_space();
                if (take(']')) {
                    return true;
                }
                if (!expect(',', path)) {
                    return false;
                }
            }
        }
        if (value == '-' ||
            (value >= '0' && value <= '9')) {
            return skip_number(path);
        }
        for (const auto literal : {
                 std::string_view("true"),
                 std::string_view("false"),
                 std::string_view("null")}) {
            if (input_.substr(position_, literal.size()) == literal) {
                position_ += literal.size();
                return true;
            }
        }
        return fail(rt::RuntimeProfileErrorCode::syntax, path);
    }

    bool skip_string(std::string_view path) noexcept {
        skip_space();
        if (!take('"')) {
            return fail(rt::RuntimeProfileErrorCode::invalid_type, path);
        }
        while (position_ < input_.size()) {
            const char value = input_[position_++];
            if (value == '"') {
                return true;
            }
            if (static_cast<unsigned char>(value) < 0x20u) {
                return fail(rt::RuntimeProfileErrorCode::syntax, path);
            }
            if (value == '\\') {
                if (position_ >= input_.size()) {
                    return fail(rt::RuntimeProfileErrorCode::syntax, path);
                }
                const char escape = input_[position_++];
                if (escape == 'u') {
                    for (int index = 0; index < 4; ++index) {
                        if (position_ >= input_.size() ||
                            !hex_digit(input_[position_++])) {
                            return fail(
                                rt::RuntimeProfileErrorCode::syntax,
                                path);
                        }
                    }
                } else if (
                    escape != '"' && escape != '\\' && escape != '/' &&
                    escape != 'b' && escape != 'f' && escape != 'n' &&
                    escape != 'r' && escape != 't') {
                    return fail(rt::RuntimeProfileErrorCode::syntax, path);
                }
            }
        }
        return fail(rt::RuntimeProfileErrorCode::syntax, path);
    }

    bool skip_number(std::string_view path) noexcept {
        const auto start = position_;
        if (take('-') && position_ >= input_.size()) {
            return fail(rt::RuntimeProfileErrorCode::syntax, path);
        }
        if (take('0')) {
            if (position_ < input_.size() &&
                input_[position_] >= '0' &&
                input_[position_] <= '9') {
                return fail(rt::RuntimeProfileErrorCode::syntax, path);
            }
        } else {
            const auto digits = position_;
            while (position_ < input_.size() &&
                   input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
            if (digits == position_) {
                return fail(rt::RuntimeProfileErrorCode::syntax, path);
            }
        }
        if (take('.')) {
            const auto digits = position_;
            while (position_ < input_.size() &&
                   input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
            if (digits == position_) {
                return fail(rt::RuntimeProfileErrorCode::syntax, path);
            }
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            const auto digits = position_;
            while (position_ < input_.size() &&
                   input_[position_] >= '0' &&
                   input_[position_] <= '9') {
                ++position_;
            }
            if (digits == position_) {
                return fail(rt::RuntimeProfileErrorCode::syntax, path);
            }
        }
        if (position_ == start) {
            return fail(rt::RuntimeProfileErrorCode::syntax, path);
        }
        return true;
    }

    bool expect(char expected, std::string_view path) noexcept {
        skip_space();
        if (!take(expected)) {
            return fail(rt::RuntimeProfileErrorCode::syntax, path);
        }
        return true;
    }

    bool take(char value) noexcept {
        if (position_ < input_.size() && input_[position_] == value) {
            ++position_;
            return true;
        }
        return false;
    }

    void skip_space() noexcept {
        while (position_ < input_.size()) {
            const char value = input_[position_];
            if (value != ' ' && value != '\t' &&
                value != '\n' && value != '\r') {
                break;
            }
            ++position_;
        }
    }

    bool fail(
        rt::RuntimeProfileErrorCode code,
        std::string_view path) noexcept {
        if (error_.code != rt::RuntimeProfileErrorCode::none) {
            return false;
        }
        error_.code = code;
        error_.byte_offset = position_;
        const auto count =
            path.size() < error_.path.size() - 1
                ? path.size()
                : error_.path.size() - 1;
        for (std::size_t index = 0; index < count; ++index) {
            error_.path[index] = path[index];
        }
        error_.path[count] = '\0';
        return false;
    }

    static bool hex_digit(char value) noexcept {
        return (value >= '0' && value <= '9') ||
               (value >= 'a' && value <= 'f') ||
               (value >= 'A' && value <= 'F');
    }

    std::string_view input_;
    std::size_t position_ = 0;
    rt::RuntimeProfileError& error_;
};

} // namespace

namespace rt {

Status parse_runtime_profile(
    std::string_view json,
    RuntimeConfig& config,
    RuntimeProfileMetadata& metadata,
    RuntimeProfileError& error) noexcept {
    RuntimeProfileError parsed_error{};
    if (json.empty()) {
        parsed_error.code = RuntimeProfileErrorCode::syntax;
        parsed_error.path[0] = '$';
        parsed_error.path[1] = '\0';
        error = parsed_error;
        return Status::invalid_config;
    }
    if (json.size() > runtime_profile_max_bytes) {
        parsed_error.code = RuntimeProfileErrorCode::input_too_large;
        parsed_error.byte_offset = json.size();
        parsed_error.path[0] = '$';
        parsed_error.path[1] = '\0';
        error = parsed_error;
        return Status::invalid_config;
    }
    std::size_t invalid_utf8_offset = 0;
    if (!valid_utf8(json, invalid_utf8_offset)) {
        parsed_error.code = RuntimeProfileErrorCode::syntax;
        parsed_error.byte_offset = invalid_utf8_offset;
        parsed_error.path[0] = '$';
        parsed_error.path[1] = '\0';
        error = parsed_error;
        return Status::invalid_config;
    }

    RuntimeConfig candidate = config;
    RuntimeProfileMetadata candidate_metadata = metadata;
    Parser parser(json, parsed_error);
    if (!parser.parse(candidate, candidate_metadata)) {
        error = parsed_error;
        if (parsed_error.code == RuntimeProfileErrorCode::incompatible_schema ||
            parsed_error.code == RuntimeProfileErrorCode::incompatible_runtime) {
            return Status::incompatible_artifact;
        }
        return Status::invalid_config;
    }

    config = candidate;
    metadata = candidate_metadata;
    error = {};
    return Status::ok;
}

const char* runtime_profile_error_message(
    RuntimeProfileErrorCode code) noexcept {
    switch (code) {
    case RuntimeProfileErrorCode::none:
        return "no profile error";
    case RuntimeProfileErrorCode::input_too_large:
        return "profile exceeds the bounded input size";
    case RuntimeProfileErrorCode::syntax:
        return "profile JSON is malformed";
    case RuntimeProfileErrorCode::duplicate_key:
        return "profile contains a duplicate key";
    case RuntimeProfileErrorCode::unknown_key:
        return "profile contains an unknown key";
    case RuntimeProfileErrorCode::missing_key:
        return "profile is missing a required key";
    case RuntimeProfileErrorCode::invalid_type:
        return "profile value has the wrong JSON type";
    case RuntimeProfileErrorCode::invalid_value:
        return "profile value violates the runtime configuration contract";
    case RuntimeProfileErrorCode::incompatible_schema:
        return "profile schema is incompatible";
    case RuntimeProfileErrorCode::incompatible_runtime:
        return "profile requires an incompatible runtime version";
    }
    return "unknown profile error";
}

} // namespace rt
