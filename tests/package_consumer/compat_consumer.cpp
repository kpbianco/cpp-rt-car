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
    const rt::DeviceBackendRegistration pre_m17_device_backend{
        "legacy.device.v1", {}};
    const rt::DeviceBufferRegistration pre_m17_device_buffer{
        "legacy.buffer.v1", {}, {}};
    const rt::ThreadPolicyReport pre_m15_04_thread{
        rt::thread_role_frame,
        {},
        {},
        rt::ResourceOwnership::caller,
        rt::PolicyApplicationMode::verify_only,
        1,
        true,
    };
    const rt::CpuMemoryPolicyReport pre_m15_04_report{
        rt::cpu_memory_policy_schema_version, 1, {pre_m15_04_thread}, 0, {}};
    const rt::MemoryPlan pre_m16_plan{1024, 512};
    const rt::CrossRateChannelRegistration additive_cross_rate{};
    const rt::RateExecutionPolicy additive_rate_execution{};
    const rt::RateExecutionPolicy pre_m16_04_rate_execution{17};
    const rt::StepResult::RateSummary pre_m16_04_summary{
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
        false, {}, 0, 0};
    const rt::HostFrameContext pre_m16_03_frame{
        1, std::chrono::nanoseconds{1}, std::nullopt};
    const rt::CompiledRateDomain pre_m16_03_compiled{
        {}, {}, 0, 1, 1, 1, 1, rt::RateCriticality::normal,
        false, 1, 1, 1};
    if (pre_m17_device_backend.name != "legacy.device.v1" ||
        pre_m17_device_backend.api.abi_version != 0 ||
        pre_m17_device_buffer.name != "legacy.buffer.v1" ||
        pre_m17_device_buffer.flags !=
            (RTFW_DEVICE_BUFFER_HOST_READ |
             RTFW_DEVICE_BUFFER_HOST_WRITE |
             RTFW_DEVICE_BUFFER_DEVICE_READ |
             RTFW_DEVICE_BUFFER_DEVICE_WRITE) ||
        pre_m15_04_report.threads[0].role != rt::thread_role_frame ||
        pre_m15_04_report.accounting_complete ||
        pre_m16_plan.rate_plan_bytes != 0 ||
        additive_cross_rate.payload_size != 0 ||
        additive_rate_execution.maximum_dispatch_records_per_step != 0 ||
        pre_m16_04_rate_execution.maximum_dispatch_records_per_step != 17 ||
        pre_m16_04_rate_execution.host_policy_version != 1 ||
        pre_m16_04_summary.optional_due_domain_releases != 0 ||
        pre_m16_04_summary.rate_policy_version != 0 ||
        pre_m16_03_frame.nominal_release_ns.has_value() ||
        pre_m16_03_compiled.late_action != rt::RateLateAction::fail) {
        return 3;
    }
    if (legacy_pipeline_factory == nullptr) {
        return 1;
    }

    const auto duration = legacy_tick_duration(core::seconds{1.0});
    return duration == core::seconds{1.0} ? 0 : 2;
}
