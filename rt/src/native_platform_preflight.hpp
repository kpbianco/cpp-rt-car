#pragma once

#include <rt/runtime.hpp>

namespace rt::detail {

class NativePlatformPreflightProbe final : public PlatformPreflightProbe {
public:
    void inspect(
        std::size_t planned_runtime_bytes,
        const RuntimeClock& clock,
        PlatformPreflightReport& report) noexcept override;
};

} // namespace rt::detail
