#pragma once

#include <rt/runtime.hpp>

namespace rt::detail {

class NativeThreadPolicyProvider final : public ThreadPolicyProvider {
public:
    [[nodiscard]] ThreadPolicyProviderCapabilities capabilities()
        const noexcept override;
    [[nodiscard]] Status apply_current_thread(
        ThreadResourceId id,
        const ThreadPolicy& policy,
        ThreadPolicy& applied,
        int& system_error) noexcept override;
    [[nodiscard]] Status inspect_current_thread(
        ThreadResourceId id,
        ThreadPolicy& observed,
        int& system_error) noexcept override;
};

} // namespace rt::detail
