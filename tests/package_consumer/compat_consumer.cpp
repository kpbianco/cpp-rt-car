#include <rt/runtime.hpp>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace {

using LegacyPipelineFactory = rt::DemoPipeline (*)(SimCore&);
using LegacyTickDuration =
    core::seconds (*)(core::seconds) noexcept;

// Volatile pointer objects make both compatibility symbols observable to the
// linker even in optimized Release consumers.
LegacyPipelineFactory volatile legacy_pipeline_factory =
    &rt::build_demo_pipeline;
LegacyTickDuration volatile legacy_tick_duration =
    &rt::tick_duration;

} // namespace

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

int main() {
    if (legacy_pipeline_factory == nullptr) {
        return 1;
    }

    const auto duration = legacy_tick_duration(core::seconds{1.0});
    return duration == core::seconds{1.0} ? 0 : 2;
}
