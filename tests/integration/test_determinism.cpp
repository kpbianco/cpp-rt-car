#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <rt/numerics.hpp>
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

std::filesystem::path make_temp_snapshot_path(const std::string_view prefix) {
    auto dir = std::filesystem::temp_directory_path();
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<std::uint64_t> dist;

    std::ostringstream oss;
    oss << prefix << std::hex << dist(gen) << ".bin";
    return dir / oss.str();
}

std::vector<std::string> make_thread_fma_args(std::size_t threads, bool use_fma) {
    std::vector<std::string> args;
    args.reserve(use_fma ? 3 : 2);
    args.emplace_back("--threads");
    args.push_back(std::to_string(threads));
    if (use_fma) {
        args.emplace_back("--fma");
    }
    return args;
}

std::vector<std::uint8_t> run_demo_collect(const std::vector<std::string> &extra_args) {
    auto binary = find_demo_binary();
    if (binary.empty()) {
        return {};
    }

    auto tmp = make_temp_snapshot_path("rtfw_demo_");

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

TEST(DeterminismE2E, GoldenReplayStablePerConfiguration) {
    std::vector<std::size_t> thread_counts{1};
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw > 1) {
        thread_counts.push_back(static_cast<std::size_t>(hw));
    }

    const std::array<bool, 2> fma_options{false, true};

    for (bool use_fma : fma_options) {
        if (use_fma && !rt::detail::kBuildAllowsFma) {
            continue;
        }
        for (std::size_t threads : thread_counts) {
            SCOPED_TRACE(::testing::Message()
                         << threads << " threads"
                         << (use_fma ? ", FMA on" : ", FMA off"));
            auto args = make_thread_fma_args(threads, use_fma);
            auto golden = run_demo_collect(args);
            ASSERT_FALSE(golden.empty());
            auto golden_hash = rt::hash64(golden);
            auto repeat = run_demo_collect(args);
            ASSERT_FALSE(repeat.empty());
            EXPECT_EQ(rt::hash64(repeat), golden_hash);
            EXPECT_EQ(repeat, golden);
        }
    }
}

TEST(DeterminismE2E, SnapshotReloadMatchesGolden) {
    std::vector<std::size_t> thread_counts{1};
    unsigned int hw = std::thread::hardware_concurrency();
    if (hw > 1) {
        thread_counts.push_back(static_cast<std::size_t>(hw));
    }

    const std::array<bool, 2> fma_options{false, true};

    for (bool use_fma : fma_options) {
        if (use_fma && !rt::detail::kBuildAllowsFma) {
            continue;
        }
        for (std::size_t threads : thread_counts) {
            SCOPED_TRACE(::testing::Message()
                         << threads << " threads"
                         << (use_fma ? ", FMA on" : ", FMA off"));
            auto args = make_thread_fma_args(threads, use_fma);
            auto golden = run_demo_collect(args);
            ASSERT_FALSE(golden.empty());
            auto golden_hash = rt::hash64(golden);

            auto tmp = make_temp_snapshot_path("rtfw_golden_");
            {
                std::ofstream out(tmp, std::ios::binary);
                ASSERT_TRUE(out.is_open())
                    << "Failed to open temp snapshot file: " << tmp;
                out.write(reinterpret_cast<const char *>(golden.data()),
                          static_cast<std::streamsize>(golden.size()));
                ASSERT_TRUE(out.good())
                    << "Failed to write golden snapshot to: " << tmp;
            }

            std::vector<std::string> replay_args;
            auto base_args = make_thread_fma_args(threads, use_fma);
            replay_args.reserve(base_args.size() + 2);
            replay_args.emplace_back("--snapshot-in");
            replay_args.push_back(tmp.string());
            replay_args.insert(replay_args.end(), base_args.begin(), base_args.end());

            auto data = run_demo_collect(replay_args);
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            ASSERT_FALSE(data.empty());
            EXPECT_EQ(rt::hash64(data), golden_hash);
            EXPECT_EQ(data, golden);
        }
    }
}

} // namespace
