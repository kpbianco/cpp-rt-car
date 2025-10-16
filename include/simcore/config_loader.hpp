#pragma once

#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace simcore {

class ConfigLoaderError : public std::runtime_error {
public:
  explicit ConfigLoaderError(const std::string &message)
      : std::runtime_error(message) {}
};

class ConfigLoader {
public:
  static constexpr int kExpectedSchemaVersion = 1;

  ConfigLoader()
      : root_(std::filesystem::path{"configs"}) {}

  explicit ConfigLoader(std::filesystem::path root)
      : root_(std::move(root)) {}

  const std::filesystem::path &root() const noexcept { return root_; }

  std::string load(const std::filesystem::path &path) const {
    const auto resolved = resolve_path(path);
    auto payload = read_file(resolved);
    ensure_schema_version(payload, resolved);
    return payload;
  }

  std::string loadNamed(std::string_view name) const {
    if (name.empty()) {
      throw ConfigLoaderError("Config name cannot be empty");
    }
    std::filesystem::path candidate{name};
    if (candidate.extension().empty()) {
      const auto filename = candidate.filename().string() + ".json";
      candidate = candidate.parent_path() / filename;
    }
    return load(candidate);
  }

  static void ensure_schema_version(const std::string &payload,
                                    const std::filesystem::path &source) {
    const std::string key = "\"schema_version\"";
    bool in_string = false;
    for (std::size_t i = 0; i < payload.size(); ++i) {
      const char ch = payload[i];
      if (ch != '\"') {
        continue;
      }

      std::size_t backslashes = 0;
      std::size_t j = i;
      while (j > 0 && payload[j - 1] == '\\') {
        ++backslashes;
        --j;
      }
      const bool escaped = (backslashes % 2) != 0;
      if (escaped) {
        continue;
      }

      if (!in_string) {
        if (payload.compare(i, key.size(), key) == 0) {
          const auto version = parse_schema_version(payload, i + key.size(), source);
          if (version != kExpectedSchemaVersion) {
            throw ConfigLoaderError(build_version_error(source, version));
          }
          return;
        }
        in_string = true;
      } else {
        in_string = false;
      }
    }

    throw ConfigLoaderError(build_missing_version_error(source));
  }

private:
  static std::string build_missing_version_error(const std::filesystem::path &source) {
    std::ostringstream oss;
    oss << "Config '" << source.string()
        << "' is missing schema_version (expected " << kExpectedSchemaVersion
        << "). Regenerate the config with the latest tooling.";
    return oss.str();
  }

  static std::string build_invalid_version_error(const std::filesystem::path &source) {
    std::ostringstream oss;
    oss << "Config '" << source.string()
        << "' has an invalid schema_version; expected integer "
        << kExpectedSchemaVersion << ".";
    return oss.str();
  }

  static std::string build_version_error(const std::filesystem::path &source,
                                         int found) {
    std::ostringstream oss;
    oss << "Config '" << source.string() << "' uses schema_version " << found
        << " but this runtime only supports version " << kExpectedSchemaVersion
        << ". Regenerate the config or update the runtime.";
    return oss.str();
  }

  static int parse_schema_version(const std::string &payload,
                                  std::size_t start,
                                  const std::filesystem::path &source) {
    const auto colon = payload.find(':', start);
    if (colon == std::string::npos) {
      throw ConfigLoaderError(build_invalid_version_error(source));
    }
    const auto value_begin = payload.find_first_not_of(" \t\r\n", colon + 1);
    if (value_begin == std::string::npos) {
      throw ConfigLoaderError(build_invalid_version_error(source));
    }
    auto value_end = value_begin;
    const bool has_sign = payload[value_end] == '+' || payload[value_end] == '-';
    if (has_sign) {
      ++value_end;
    }
    while (value_end < payload.size() &&
           std::isdigit(static_cast<unsigned char>(payload[value_end]))) {
      ++value_end;
    }
    if (value_end == value_begin || (has_sign && value_end == value_begin + 1)) {
      throw ConfigLoaderError(build_invalid_version_error(source));
    }

    int parsed = 0;
    const auto *first = payload.data() + value_begin;
    const auto *last = payload.data() + value_end;
    const auto result = std::from_chars(first, last, parsed);
    if (result.ec != std::errc{} || result.ptr != last) {
      throw ConfigLoaderError(build_invalid_version_error(source));
    }
    return parsed;
  }

  std::filesystem::path resolve_path(const std::filesystem::path &path) const {
    if (path.is_absolute() || root_.empty()) {
      return path;
    }
    return root_ / path;
  }

  static std::string read_file(const std::filesystem::path &path) {
    std::ifstream in(path);
    if (!in.is_open()) {
      std::ostringstream oss;
      oss << "Failed to open config '" << path.string() << "'.";
      throw ConfigLoaderError(oss.str());
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (!in.good() && !in.eof()) {
      std::ostringstream oss;
      oss << "Failed to read config '" << path.string() << "'.";
      throw ConfigLoaderError(oss.str());
    }
    return buffer.str();
  }

  std::filesystem::path root_;
};

} // namespace simcore
