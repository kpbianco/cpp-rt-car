#include <gtest/gtest.h>

#include <simcore/SimCore.hpp>
#include <simcore/metrics.hpp>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

enum class JsonType { kNull, kBoolean, kNumber, kString, kArray, kObject };

struct JsonValue {
  JsonType type = JsonType::kNull;
  std::map<std::string, JsonValue> object;
  std::vector<JsonValue> array;
  std::string string;
  double number = 0.0;
  bool boolean = false;
  bool numberIsInteger = false;

  [[nodiscard]] bool isObject() const { return type == JsonType::kObject; }
  [[nodiscard]] bool isNumber() const { return type == JsonType::kNumber; }
};

class JsonParser {
public:
  explicit JsonParser(std::string_view text) : text_(text) {}

  JsonValue parse() {
    JsonValue value = parseValue();
    skipWhitespace();
    if (pos_ != text_.size()) {
      throw std::runtime_error("Unexpected trailing characters in JSON");
    }
    return value;
  }

private:
  JsonValue parseValue() {
    skipWhitespace();
    if (pos_ >= text_.size()) {
      throw std::runtime_error("Unexpected end of JSON input");
    }

    char c = text_[pos_];
    switch (c) {
    case '{':
      return parseObject();
    case '[':
      return parseArray();
    case '"':
      return parseStringValue();
    case 't':
      return parseLiteral("true", JsonType::kBoolean, true);
    case 'f':
      return parseLiteral("false", JsonType::kBoolean, false);
    case 'n':
      return parseLiteral("null", JsonType::kNull);
    default:
      if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
        return parseNumber();
      }
      break;
    }

    throw std::runtime_error("Invalid JSON value");
  }

  void skipWhitespace() {
    while (pos_ < text_.size() &&
           std::isspace(static_cast<unsigned char>(text_[pos_]))) {
      ++pos_;
    }
  }

  void consume(char expected) {
    if (pos_ >= text_.size() || text_[pos_] != expected) {
      throw std::runtime_error("Unexpected character in JSON");
    }
    ++pos_;
  }

  JsonValue parseObject() {
    consume('{');
    JsonValue value;
    value.type = JsonType::kObject;
    skipWhitespace();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return value;
    }

    while (true) {
      skipWhitespace();
      std::string key = parseString();
      skipWhitespace();
      consume(':');
      JsonValue element = parseValue();
      value.object.emplace(std::move(key), std::move(element));
      skipWhitespace();

      if (pos_ >= text_.size()) {
        throw std::runtime_error("Unterminated JSON object");
      }

      char c = text_[pos_];
      if (c == ',') {
        ++pos_;
        continue;
      }
      if (c == '}') {
        ++pos_;
        break;
      }
      throw std::runtime_error("Expected ',' or '}' in JSON object");
    }

    return value;
  }

  JsonValue parseArray() {
    consume('[');
    JsonValue value;
    value.type = JsonType::kArray;
    skipWhitespace();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return value;
    }

    while (true) {
      value.array.push_back(parseValue());
      skipWhitespace();

      if (pos_ >= text_.size()) {
        throw std::runtime_error("Unterminated JSON array");
      }

      char c = text_[pos_];
      if (c == ',') {
        ++pos_;
        continue;
      }
      if (c == ']') {
        ++pos_;
        break;
      }
      throw std::runtime_error("Expected ',' or ']' in JSON array");
    }

    return value;
  }

  JsonValue parseStringValue() {
    JsonValue value;
    value.type = JsonType::kString;
    value.string = parseString();
    return value;
  }

  JsonValue parseLiteral(std::string_view literal, JsonType type,
                         bool boolValue = false) {
    if (text_.substr(pos_, literal.size()) != literal) {
      throw std::runtime_error("Invalid literal in JSON");
    }
    pos_ += literal.size();
    JsonValue value;
    value.type = type;
    if (type == JsonType::kBoolean) {
      value.boolean = boolValue;
    }
    return value;
  }

  JsonValue parseNumber() {
    const std::size_t start = pos_;
    if (text_[pos_] == '-') {
      ++pos_;
    }

    if (pos_ >= text_.size()) {
      throw std::runtime_error("Invalid JSON number");
    }

    if (text_[pos_] == '0') {
      ++pos_;
    } else {
      if (!std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        throw std::runtime_error("Invalid JSON number");
      }
      while (pos_ < text_.size() &&
             std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        ++pos_;
      }
    }

    bool isInteger = true;
    if (pos_ < text_.size() && text_[pos_] == '.') {
      isInteger = false;
      ++pos_;
      if (pos_ >= text_.size() ||
          !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        throw std::runtime_error("Invalid fractional part in JSON number");
      }
      while (pos_ < text_.size() &&
             std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        ++pos_;
      }
    }

    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      isInteger = false;
      ++pos_;
      if (pos_ < text_.size() &&
          (text_[pos_] == '+' || text_[pos_] == '-')) {
        ++pos_;
      }
      if (pos_ >= text_.size() ||
          !std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        throw std::runtime_error("Invalid exponent in JSON number");
      }
      while (pos_ < text_.size() &&
             std::isdigit(static_cast<unsigned char>(text_[pos_]))) {
        ++pos_;
      }
    }

    const std::string raw(text_.substr(start, pos_ - start));
    char *end = nullptr;
    double value = std::strtod(raw.c_str(), &end);
    if (end == nullptr || *end != '\0') {
      throw std::runtime_error("Failed to parse JSON number");
    }

    JsonValue result;
    result.type = JsonType::kNumber;
    result.number = value;
    result.numberIsInteger = isInteger;
    return result;
  }

  std::string parseString() {
    consume('"');
    std::string result;
    while (pos_ < text_.size()) {
      char c = text_[pos_++];
      if (c == '"') {
        return result;
      }
      if (c == '\\') {
        if (pos_ >= text_.size()) {
          throw std::runtime_error("Invalid escape sequence in JSON string");
        }
        char escaped = text_[pos_++];
        switch (escaped) {
        case '"':
        case '\\':
        case '/':
          result.push_back(escaped);
          break;
        case 'b':
          result.push_back('\b');
          break;
        case 'f':
          result.push_back('\f');
          break;
        case 'n':
          result.push_back('\n');
          break;
        case 'r':
          result.push_back('\r');
          break;
        case 't':
          result.push_back('\t');
          break;
        case 'u': {
          char32_t codepoint = parseCodeUnit();
          if (0xD800 <= codepoint && codepoint <= 0xDBFF) {
            if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' ||
                text_[pos_ + 1] != 'u') {
              throw std::runtime_error("Invalid Unicode surrogate pair");
            }
            pos_ += 2;
            char32_t low = parseCodeUnit();
            if (low < 0xDC00 || low > 0xDFFF) {
              throw std::runtime_error("Invalid Unicode surrogate pair");
            }
            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) +
                        (low - 0xDC00);
          } else if (0xDC00 <= codepoint && codepoint <= 0xDFFF) {
            throw std::runtime_error("Invalid Unicode surrogate");
          }
          appendCodepoint(result, codepoint);
          break;
        }
        default:
          throw std::runtime_error("Unsupported escape sequence in JSON string");
        }
      } else {
        result.push_back(c);
      }
    }
    throw std::runtime_error("Unterminated JSON string");
  }

  char32_t parseCodeUnit() {
    if (pos_ + 4 > text_.size()) {
      throw std::runtime_error("Incomplete Unicode escape in JSON string");
    }
    char32_t codepoint = 0;
    for (int i = 0; i < 4; ++i) {
      codepoint = (codepoint << 4) | hexValue(text_[pos_++]);
    }
    return codepoint;
  }

  static unsigned hexValue(char c) {
    if (c >= '0' && c <= '9') {
      return static_cast<unsigned>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
      return static_cast<unsigned>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
      return static_cast<unsigned>(c - 'A' + 10);
    }
    throw std::runtime_error("Invalid hex digit in Unicode escape");
  }

  static void appendCodepoint(std::string &out, char32_t codepoint) {
    if (codepoint <= 0x7F) {
      out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
      out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

} // namespace


TEST(MetricsJson, EmitsPhaseStatsAndCounters) {
  SimCore::Settings settings;
  settings.hz = 240.0;
  settings.maxFrames = 6;
  settings.threads = 2;
  settings.predictiveEnable = false;
  settings.adaptive = false;

  SimCore sim(settings);
  metrics::Registry registry;
  sim.setMetrics(&registry);

  auto input = sim.addPhase("Input");
  auto physics = sim.addPhase("Physics");
  constexpr std::size_t kEntityCount = 32;
  sim.setPhaseElementCount(physics, kEntityCount);

  std::vector<double> control(kEntityCount, 0.0);
  std::vector<double> state(kEntityCount, 1.0);

  sim.addSerialSubsystem(input,
                         [&](int64_t frame, SimCore::Seconds dt) {
                           for (std::size_t i = 0; i < control.size(); ++i) {
                             control[i] = std::sin(static_cast<double>(frame) *
                                                   dt.count() +
                                                   static_cast<double>(i) * 0.05);
                           }
                         });

  sim.addParallelRangeTask(
      physics,
      [&](std::size_t begin, std::size_t end, int64_t, SimCore::Seconds) {
        for (std::size_t i = begin; i < end; ++i) {
          state[i] += control[i] * 0.5;
        }
      });

  sim.run();

  auto snapshot = registry.snapshot();
  ASSERT_GE(snapshot.phases.size(), 2u);
  auto inputIt = snapshot.phases.find("Input");
  ASSERT_NE(inputIt, snapshot.phases.end());
  EXPECT_GT(inputIt->second.samples, 0u);

  auto physicsIt = snapshot.phases.find("Physics");
  ASSERT_NE(physicsIt, snapshot.phases.end());
  EXPECT_GT(physicsIt->second.samples, 0u);

  auto counters = snapshot.counters;
  EXPECT_NE(counters.find("missed_frames"), counters.end());
  EXPECT_NE(counters.find("watchdog.trips"), counters.end());
  EXPECT_NE(counters.find("worker.queue_max"), counters.end());
  EXPECT_NE(counters.find("worker.steals_total"), counters.end());
  EXPECT_NE(counters.find("logger.dropped"), counters.end());
  EXPECT_NE(counters.find("trace.dropped"), counters.end());
  EXPECT_NE(counters.find("log_drops"), counters.end());
  EXPECT_NE(counters.find("thermal.events"), counters.end());

  const std::string json = registry.to_json();
  EXPECT_FALSE(json.empty());
  EXPECT_EQ(json.front(), '{');
  EXPECT_EQ(json.back(), '}');
  EXPECT_EQ(json.find('\n'), std::string::npos);
  EXPECT_EQ(json.find('\r'), std::string::npos);

  JsonValue root;
  ASSERT_NO_THROW({ root = JsonParser(json).parse(); });
  ASSERT_TRUE(root.isObject());
  EXPECT_EQ(root.object.size(), 2u);

  auto phasesJsonIt = root.object.find("phases");
  ASSERT_NE(phasesJsonIt, root.object.end());
  ASSERT_TRUE(phasesJsonIt->second.isObject());
  const auto &phasesJson = phasesJsonIt->second.object;
  EXPECT_GE(phasesJson.size(), 2u);

  auto validatePhase = [&](const std::pair<const std::string, JsonValue> &entry) {
    const JsonValue &phaseValue = entry.second;
    ASSERT_TRUE(phaseValue.isObject());
    EXPECT_EQ(phaseValue.object.size(), 3u);

    auto checkPercentile = [&](std::string_view key) {
      auto it = phaseValue.object.find(std::string(key));
      ASSERT_NE(it, phaseValue.object.end());
      ASSERT_TRUE(it->second.isNumber());
      EXPECT_TRUE(std::isfinite(it->second.number));
      EXPECT_GE(it->second.number, 0.0);
    };

    checkPercentile("p50_ms");
    checkPercentile("p95_ms");
    checkPercentile("p99_ms");
  };

  auto inputJsonIt = phasesJson.find("Input");
  ASSERT_NE(inputJsonIt, phasesJson.end());
  validatePhase(*inputJsonIt);

  auto physicsJsonIt = phasesJson.find("Physics");
  ASSERT_NE(physicsJsonIt, phasesJson.end());
  validatePhase(*physicsJsonIt);

  auto countersJsonIt = root.object.find("counters");
  ASSERT_NE(countersJsonIt, root.object.end());
  ASSERT_TRUE(countersJsonIt->second.isObject());
  const auto &countersJson = countersJsonIt->second.object;
  EXPECT_FALSE(countersJson.empty());

  for (const auto &kv : countersJson) {
    ASSERT_TRUE(kv.second.isNumber());
    EXPECT_TRUE(kv.second.numberIsInteger);
    EXPECT_GE(kv.second.number, 0.0);
  }

  auto expectCounter = [&](std::string_view key) {
    auto it = countersJson.find(std::string(key));
    ASSERT_NE(it, countersJson.end());
  };

  expectCounter("missed_frames");
  expectCounter("watchdog.trips");
  expectCounter("worker.queue_max");
  expectCounter("worker.steals_total");
  expectCounter("logger.dropped");
  expectCounter("trace.dropped");
  expectCounter("log_drops");
  expectCounter("thermal.events");
}
