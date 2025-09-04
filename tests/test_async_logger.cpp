#include <gtest/gtest.h>
#include <simcore/logger.hpp>
#include <fstream>
#include <filesystem>
#include <thread>

TEST(Logger, AsyncRingFileSinkFlushes)
{
    namespace fs = std::filesystem;
    auto path = fs::temp_directory_path() / "async_log_test.log";
    {
        Logger log;
        auto sink = std::make_shared<Logger::AsyncRingFileSink>(path.string(), 16, std::chrono::milliseconds(10));
        log.addSink(sink);
        for(int i=0;i<10;++i){
            log.info("msg{}", i);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::ifstream in(path);
    std::string line;
    int count = 0;
    while(std::getline(in,line)) ++count;
    EXPECT_EQ(count,10);
    fs::remove(path);
}
