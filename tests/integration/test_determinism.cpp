#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include <system_error>

#include <rt/snapshot.hpp>

namespace {

std::string quote_arg(const std::string &arg) {
#ifdef _WIN32
    std::string quoted;
    quoted.reserve(arg.size() + 2);
    quoted.push_back('"');
    for (char c : arg) {
        if (c == '"') {
            quoted.push_back('"');
            quoted.push_back('"');
        } else {
            quoted.push_back(c);
        }
    }
    quoted.push_back('"');
    return quoted;
#else
    std::string quoted;
    quoted.reserve(arg.size() + 2);
    quoted.push_back('\'');
    for (char c : arg) {
        if (c == '\'') {
            quoted.append("'\"'\"'");
        } else {
            quoted.push_back(c);
        }
    }
    quoted.push_back('\'');
    return quoted;
#endif
}

std::filesystem::path find_demo_binary() {
    std::filesystem::path base(PROJECT_BINARY_DIR);
    std::string cfg = CMAKE_CFG_INTDIR;
    const bool has_cfg = !cfg.empty() && cfg != "." && cfg.find('$') == std::string::npos;

    auto candidate_paths = std::vector<std::filesystem::path>{};
    candidate_paths.reserve(4);

    auto add_candidate = [&](const std::filesystem::path &prefix) {
        std::filesystem::path p = prefix / "rtfw_demo";
#ifdef _WIN32
        p.replace_extension(".exe");
#endif
        candidate_paths.push_back(p);
        std::filesystem::path bin = prefix / "bin" / "rtfw_demo";
#ifdef _WIN32
        bin.replace_extension(".exe");
#endif
        candidate_paths.push_back(bin);
    };

    if (has_cfg) {
        add_candidate(base / cfg);
    }
    add_candidate(base);

    for (const auto &candidate : candidate_paths) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return candidate;
        }
    }

    ADD_FAILURE() << "Failed to locate rtfw_demo binary";
    return {};
}

std::vector<std::uint8_t> read_binary(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> run_demo_collect(const std::vector<std::string> &extra_args) {
    auto binary = find_demo_binary();
    if (binary.empty()) {
        return {};
    }

    auto tmp = std::filesystem::temp_directory_path() /
               std::filesystem::unique_path("rtfw_demo_%%%%%%%%.bin");

    std::vector<std::string> args;
    args.reserve(extra_args.size() + 4);
    args.push_back(binary.string());
    args.insert(args.end(), extra_args.begin(), extra_args.end());
    args.push_back("--snapshot-out");
    args.push_back(tmp.string());

    std::string command;
    command.reserve(128);
    for (const auto &arg : args) {
        if (!command.empty()) command.push_back(' ');
        command += quote_arg(arg);
    }

    int rc = std::system(command.c_str());
    EXPECT_EQ(rc, 0) << "Command failed: " << command;

    auto data = read_binary(tmp);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    EXPECT_FALSE(data.empty()) << "Snapshot missing: " << tmp;
    return data;
}

TEST(DeterminismE2E, GoldenReplayMatchesSingleAndMultiThread) {
    const auto golden = run_demo_collect({"--threads", "1"});
    ASSERT_FALSE(golden.empty());
    const auto golden_hash = rt::hash64(golden);

    std::vector<std::size_t> thread_counts{1};
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw > 1) {
        thread_counts.push_back(static_cast<std::size_t>(hw));
    }

    for (std::size_t threads : thread_counts) {
        auto data = run_demo_collect({"--threads", std::to_string(threads)});
        ASSERT_FALSE(data.empty());
        EXPECT_EQ(rt::hash64(data), golden_hash)
            << "Hash mismatch for " << threads << " threads";
        EXPECT_EQ(data, golden) << "Snapshot bytes differ for " << threads << " threads";
    }
}

TEST(DeterminismE2E, SnapshotReloadMatchesGolden) {
    const auto golden = run_demo_collect({"--threads", "1"});
    ASSERT_FALSE(golden.empty());
    const auto golden_hash = rt::hash64(golden);

    auto tmp = std::filesystem::temp_directory_path() /
               std::filesystem::unique_path("rtfw_golden_%%%%%%%%.bin");
    {
        std::ofstream out(tmp, std::ios::binary);
        ASSERT_TRUE(out.is_open()) << "Failed to open temp snapshot file: " << tmp;
        out.write(reinterpret_cast<const char *>(golden.data()),
                  static_cast<std::streamsize>(golden.size()));
        ASSERT_TRUE(out.good()) << "Failed to write golden snapshot to: " << tmp;
    }

    auto data = run_demo_collect({"--snapshot-in", tmp.string(), "--threads", "1"});
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    ASSERT_FALSE(data.empty());
    EXPECT_EQ(rt::hash64(data), golden_hash);
    EXPECT_EQ(data, golden);
}

} // namespace
