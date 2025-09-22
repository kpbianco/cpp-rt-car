#include "api/cabi.h"

#include <simcore/SimCore.hpp>
#include <simcore/logger.hpp>

#include <cmath>
#include <exception>
#include <memory>
#include <new>

struct rtfw_handle {
    Logger logger;
    std::unique_ptr<SimCore> sim;
};

namespace {

template <typename Tag, typename Tag::type M>
struct StepFriend {
    friend typename Tag::type get(Tag) {
        return M;
    }
};

struct StepAccessor {
    using type = bool (SimCore::*)();
    friend type get(StepAccessor);
};

template struct StepFriend<StepAccessor, &SimCore::step>;

constexpr double kDefaultHz = 500.0;

SimCore::Settings make_settings(double frame_budget_ms) {
    SimCore::Settings settings;
    settings.threads = 1;
    settings.maxFrames = -1;
    settings.driftLogInterval = 0;
    settings.budgetMonitor = false;
    settings.rateGovernorEnable = false;
    settings.predictiveEnable = false;
    settings.autoTuneChunks = false;
    settings.budgetWindow = 0;

    if (std::isfinite(frame_budget_ms) && frame_budget_ms > 0.0) {
        settings.hz = 1000.0 / frame_budget_ms;
    } else {
        settings.hz = kDefaultHz;
    }

    return settings;
}

bool advance_frame(SimCore &sim) {
    auto fn = get(StepAccessor{});
    return (sim.*fn)();
}

} // namespace

extern "C" {

RTFW_API rtfw_handle *rtfw_create(double frame_budget_ms) {
    auto handle = std::unique_ptr<rtfw_handle>(new (std::nothrow) rtfw_handle());
    if (!handle) {
        return nullptr;
    }
    try {
        auto settings = make_settings(frame_budget_ms);
        auto sim = std::make_unique<SimCore>(settings);
        handle->logger.setLevel(Logger::Level::Error);
        sim->setLogger(&handle->logger);
        handle->sim = std::move(sim);
    } catch (const std::exception &) {
        return nullptr;
    } catch (...) {
        return nullptr;
    }
    return handle.release();
}

RTFW_API rtfw_status rtfw_step(rtfw_handle *handle) {
    if (!handle || !handle->sim) {
        return RTFW_STATUS_INVALID_ARGUMENT;
    }
    try {
        const bool more = advance_frame(*handle->sim);
        return more ? RTFW_STATUS_OK : RTFW_STATUS_COMPLETE;
    } catch (const std::exception &) {
        return RTFW_STATUS_INTERNAL_ERROR;
    } catch (...) {
        return RTFW_STATUS_INTERNAL_ERROR;
    }
}

RTFW_API void rtfw_destroy(rtfw_handle *handle) {
    delete handle;
}

} // extern "C"
