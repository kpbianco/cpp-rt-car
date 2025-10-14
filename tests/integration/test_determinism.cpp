#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifndef RTFW_DEMO_PATH
#error "RTFW_DEMO_PATH must be defined"
#endif

namespace {

std::string quoteArg(const std::string &arg) {
    std::string quoted = "\"";
    for (char c : arg) {
        if (c == '\\' || c == '\"')
            quoted.push_back('\\');
        quoted.push_back(c);
    }
    quoted.push_back('\"');
    return quoted;
}

std::filesystem::path makeTempFile(const std::string &tag) {
    static std::atomic<std::uint64_t> counter{0};
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto id = counter.fetch_add(1, std::memory_order_relaxed);
    auto base = std::filesystem::temp_directory_path();
    std::ostringstream oss;
    oss << "rtfw_demo_" << tag << "_" << stamp << "_" << id << ".bin";
    return base / oss.str();
}

void ensureRemoved(const std::filesystem::path &path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

std::vector<std::uint8_t> readFile(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("Failed to open file: " + path.string());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
}

std::uint64_t hashBytes(const std::vector<std::uint8_t> &bytes) {
    constexpr std::uint64_t fnvOffset = 1469598103934665603ull;
    constexpr std::uint64_t fnvPrime = 1099511628211ull;
    std::uint64_t hash = fnvOffset;
    for (std::uint8_t b : bytes) {
        hash ^= static_cast<std::uint64_t>(b);
        hash *= fnvPrime;
    }
    return hash;
}

std::string diffSummary(const std::vector<std::uint8_t> &lhs,
                        const std::vector<std::uint8_t> &rhs) {
    std::ostringstream oss;
    oss << "lhs.size=" << lhs.size() << " rhs.size=" << rhs.size();
    const std::size_t limit = std::min(lhs.size(), rhs.size());
    for (std::size_t i = 0; i < limit; ++i) {
        if (lhs[i] != rhs[i]) {
            oss << " first_mismatch=" << i
                << " lhs_byte=" << static_cast<int>(lhs[i])
                << " rhs_byte=" << static_cast<int>(rhs[i]);
            return oss.str();
        }
    }
    if (lhs.size() != rhs.size())
        oss << " (sizes differ)";
    else
        oss << " (no mismatch detected)";
    return oss.str();
}

void runDemo(const std::vector<std::string> &args) {
    std::filesystem::path binary{RTFW_DEMO_PATH};
    ASSERT_TRUE(std::filesystem::exists(binary))
        << "rtfw_demo binary missing at " << binary;

    std::string cmd = quoteArg(binary.string());
    for (const auto &arg : args) {
        cmd.push_back(' ');
        cmd += quoteArg(arg);
    }

    int rc = std::system(cmd.c_str());
    ASSERT_EQ(rc, 0) << "Command failed (" << rc << "): " << cmd;
}

} // namespace

TEST(DeterminismIntegration, SnapshotReplayMatchesGolden) {
    auto baselinePath = makeTempFile("baseline");
    auto reloadPath = makeTempFile("reload");
    auto threadedPath = makeTempFile("threaded");

    runDemo({"--threads", "1", "--snapshot-out", baselinePath.string()});

    std::vector<std::uint8_t> baseline;
    ASSERT_NO_THROW(baseline = readFile(baselinePath));
    ASSERT_FALSE(baseline.empty());

    runDemo({"--snapshot-in", baselinePath.string(), "--snapshot-out", reloadPath.string()});
    std::vector<std::uint8_t> reload;
    ASSERT_NO_THROW(reload = readFile(reloadPath));
    EXPECT_EQ(hashBytes(baseline), hashBytes(reload)) << diffSummary(baseline, reload);
    EXPECT_EQ(baseline, reload) << diffSummary(baseline, reload);

    std::size_t threads = std::thread::hardware_concurrency();
    if (threads < 2)
        threads = 2;

    runDemo({"--snapshot-in", baselinePath.string(),
             "--threads", std::to_string(threads),
             "--snapshot-out", threadedPath.string()});
    std::vector<std::uint8_t> threaded;
    ASSERT_NO_THROW(threaded = readFile(threadedPath));
    EXPECT_EQ(hashBytes(baseline), hashBytes(threaded)) << diffSummary(baseline, threaded);
    EXPECT_EQ(baseline, threaded) << diffSummary(baseline, threaded);

    ensureRemoved(baselinePath);
    ensureRemoved(reloadPath);
    ensureRemoved(threadedPath);
}
