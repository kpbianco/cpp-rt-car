#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "logger.hpp"
#include "mocks/mock_sink.hpp"

using ::testing::_;
using ::testing::Exactly;

TEST(Logger, RespectsLevel) {
    Logger log;
    auto sink = std::make_shared<MockSink>();
    log.addSink(sink);
    log.setLevel(Logger::Level::Info);

    EXPECT_CALL(*sink, write(_))
        .Times(Exactly(1))
        .WillOnce([](const Logger::Record& r){
            EXPECT_EQ(r.level, Logger::Level::Info);
            EXPECT_NE(r.msg.find("Shown"), std::string::npos);
        });

    log.debug("Hidden");  // not emitted
    log.info("Shown");
}
