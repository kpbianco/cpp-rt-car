#pragma once
#include "logger.hpp"
#include <gmock/gmock.h>

class MockSink : public Logger::Sink {
public:
    MOCK_METHOD(void, write, (const Logger::Record&), (override));
};
