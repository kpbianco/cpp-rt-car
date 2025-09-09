#include <gtest/gtest.h>
#include <simcore/logger.hpp>
#include <fstream>
#include <filesystem>
#include <thread>
#include <barrier>
#include <vector>

TEST(Logger, AsyncRingFileSinkFlushes)
{
    namespace fs = std::filesystem;
    auto path = fs::temp_directory_path() / "async_log_test.log";
    std::error_code ec;
    fs::remove(path, ec); // ensure clean slate
    {
        Logger log;
        auto sink = std::make_shared<Logger::AsyncRingFileSink>(path.string(), 16, std::chrono::milliseconds(10));
        log.addSink(sink);
        for(int i=0;i<10;++i){
            log.info("msg{}", i);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::string line;
    int count = 0;
    // polling loop to account for slower flushes on some platforms
    for (int i = 0; i < 100 && count < 10; ++i) {
        std::ifstream in(path);
        count = 0;
        while (std::getline(in, line)) ++count;
        if (count == 10) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(count,10);
    fs::remove(path);
}

TEST(Logger, AsyncRingFileSinkDrops)
{
    namespace fs = std::filesystem;
    auto path = fs::temp_directory_path() / "async_log_drop.log";
    std::error_code ec;
    fs::remove(path, ec);
    auto sink = std::make_shared<Logger::AsyncRingFileSink>(path.string(), 1,
                                                           std::chrono::milliseconds(100));
    {
        Logger log;
        log.addSink(sink);
        constexpr int threads = 4;
        constexpr int perThread = 1000;
        std::barrier start(threads + 1);
        std::vector<std::thread> workers;
        for (int t = 0; t < threads; ++t) {
            workers.emplace_back([&] {
                start.arrive_and_wait();
                for (int i = 0; i < perThread; ++i) {
                    log.info("msg{}", i);
                }
            });
        }
        start.arrive_and_wait();
        for (auto &th : workers) th.join();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_GT(sink->dropped(), 0u);
    fs::remove(path);
}
