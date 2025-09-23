#include <gtest/gtest.h>

#include <simcore/SimCore.hpp>
#include <simcore/metrics.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::size_t findMatchingBrace(std::string_view json, std::size_t open) {
  int depth = 0;
  bool inString = false;
  bool escape = false;
  for (std::size_t i = open; i < json.size(); ++i) {
    char c = json[i];
    if (inString) {
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        inString = false;
      }
      continue;
    }
    if (c == '"') {
      inString = true;
      continue;
    }
    if (c == '{') {
      ++depth;
      continue;
    }
    if (c == '}') {
      --depth;
      if (depth == 0) {
        return i;
      }
      continue;
    }
  }
  return std::string_view::npos;
}

std::string_view extractMemberObject(std::string_view json, std::string_view key) {
  std::string pattern = std::string("\"") + std::string(key) + "\":{";
  auto pos = json.find(pattern);
  if (pos == std::string_view::npos) {
    return {};
  }
  std::size_t open = pos + pattern.size() - 1;
  auto close = findMatchingBrace(json, open);
  if (close == std::string_view::npos) {
    return {};
  }
  return json.substr(open, close - open + 1);
}

struct NumberToken {
  double value = 0.0;
  bool isInteger = false;
};

std::optional<NumberToken> parseNumberField(std::string_view json,
                                            std::string_view key) {
  std::string pattern = std::string("\"") + std::string(key) + "\":";
  auto pos = json.find(pattern);
  if (pos == std::string_view::npos) {
    return std::nullopt;
  }
  pos += pattern.size();
  while (pos < json.size() && json[pos] == ' ') {
    ++pos;
  }
  if (pos >= json.size() || json[pos] == '+') {
    return std::nullopt;
  }
  std::size_t end = pos;
  while (end < json.size()) {
    char c = json[end];
    if ((c >= '0' && c <= '9') || c == '-' || c == '.' || c == 'e' ||
        c == 'E') {
      ++end;
      continue;
    }
    if (c == '+' && end > pos) {
      ++end;
      continue;
    }
    break;
  }
  if (end == pos || end >= json.size()) {
    return std::nullopt;
  }
  char delimiter = json[end];
  if (delimiter != ',' && delimiter != '}' && delimiter != ']') {
    return std::nullopt;
  }
  std::string token(json.substr(pos, end - pos));
  char *parseEnd = nullptr;
  double value = std::strtod(token.c_str(), &parseEnd);
  if (parseEnd != token.c_str() + token.size()) {
    return std::nullopt;
  }
  bool isInteger = token.find_first_of(".eE") == std::string::npos;
  return NumberToken{value, isInteger};
}

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

  const std::string_view jsonView(json);
  const std::string phasesKey = "\"phases\":";
  auto phasesKeyPos = jsonView.find(phasesKey);
  ASSERT_NE(phasesKeyPos, std::string_view::npos);
  auto phasesOpen = jsonView.find('{', phasesKeyPos + phasesKey.size());
  ASSERT_NE(phasesOpen, std::string_view::npos);
  auto phasesClose = findMatchingBrace(jsonView, phasesOpen);
  ASSERT_NE(phasesClose, std::string_view::npos);
  ASSERT_LT(phasesClose + 1, jsonView.size());
  EXPECT_EQ(jsonView[phasesClose + 1], ',');

  const std::string countersKey = "\"counters\":";
  auto countersKeyPos = jsonView.find(countersKey, phasesClose + 2);
  ASSERT_EQ(countersKeyPos, phasesClose + 2);
  auto countersOpen = countersKeyPos + countersKey.size();
  ASSERT_LT(countersOpen, jsonView.size());
  ASSERT_EQ(jsonView[countersOpen], '{');
  auto countersClose = findMatchingBrace(jsonView, countersOpen);
  ASSERT_NE(countersClose, std::string_view::npos);
  EXPECT_EQ(countersClose, jsonView.size() - 2);

  auto phasesJson = jsonView.substr(phasesOpen, phasesClose - phasesOpen + 1);
  auto countersJson = jsonView.substr(countersOpen, countersClose - countersOpen + 1);

  auto expectPhase = [&](std::string_view name) {
    auto phaseJson = extractMemberObject(phasesJson, name);
    ASSERT_FALSE(phaseJson.empty()) << name;
    EXPECT_EQ(std::count(phaseJson.begin(), phaseJson.end(), ':'), 3);

    auto checkPercentile = [&](std::string_view percentileKey) {
      auto parsed = parseNumberField(phaseJson, percentileKey);
      ASSERT_TRUE(parsed.has_value()) << name << " missing " << percentileKey;
      EXPECT_TRUE(std::isfinite(parsed->value));
      EXPECT_GE(parsed->value, 0.0);
    };

    checkPercentile("p50_ms");
    checkPercentile("p95_ms");
    checkPercentile("p99_ms");
  };

  expectPhase("Input");
  expectPhase("Physics");

  ASSERT_GT(countersJson.size(), 2u);
  EXPECT_EQ(countersJson.front(), '{');
  EXPECT_EQ(countersJson.back(), '}');

  std::vector<std::string> seenCounters;
  std::size_t pos = 1;
  while (pos + 1 < countersJson.size()) {
    auto keyStart = countersJson.find('"', pos);
    if (keyStart == std::string_view::npos || keyStart + 1 >= countersJson.size()) {
      break;
    }
    auto keyEnd = countersJson.find('"', keyStart + 1);
    ASSERT_NE(keyEnd, std::string_view::npos);
    std::string key(countersJson.substr(keyStart + 1, keyEnd - keyStart - 1));

    auto colon = countersJson.find(':', keyEnd + 1);
    ASSERT_NE(colon, std::string_view::npos);
    auto valueStart = colon + 1;
    auto valueEnd = valueStart;
    while (valueEnd < countersJson.size() &&
           countersJson[valueEnd] >= '0' && countersJson[valueEnd] <= '9') {
      ++valueEnd;
    }
    ASSERT_GT(valueEnd, valueStart);

    std::string token(countersJson.substr(valueStart, valueEnd - valueStart));
    char *parseEnd = nullptr;
    double parsedValue = std::strtod(token.c_str(), &parseEnd);
    ASSERT_NE(parseEnd, token.c_str());
    ASSERT_EQ(*parseEnd, '\0');
    EXPECT_EQ(token.find_first_not_of("0123456789"), std::string::npos);
    EXPECT_GE(parsedValue, 0.0);

    seenCounters.push_back(std::move(key));

    if (valueEnd >= countersJson.size()) {
      break;
    }
    char terminator = countersJson[valueEnd];
    if (terminator == ',') {
      pos = valueEnd + 1;
      continue;
    }
    if (terminator == '}') {
      break;
    }
    FAIL() << "Unexpected character '" << terminator << "' in counters JSON";
  }

  EXPECT_FALSE(seenCounters.empty());

  auto expectCounter = [&](std::string_view name) {
    EXPECT_NE(std::find(seenCounters.begin(), seenCounters.end(), std::string(name)),
              seenCounters.end())
        << "Missing counter " << name;
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
