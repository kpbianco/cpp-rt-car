#include <rt/runtime.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {

struct HostQueue {
    std::array<rt::HostExecutorJob, 8> jobs{};
    std::size_t head = 0;
    std::size_t tail = 0;
    std::size_t count = 0;
    std::uint32_t next_worker = 0;

    static rt::Status submit(
        void* opaque,
        const rt::HostExecutorJob& job) noexcept {
        auto& self = *static_cast<HostQueue*>(opaque);
        if (self.count == self.jobs.size()) {
            return rt::Status::queue_full;
        }
        self.jobs[self.tail] = job;
        self.tail = (self.tail + 1) % self.jobs.size();
        ++self.count;
        return rt::Status::ok;
    }

    static bool try_execute_one(void* opaque) noexcept {
        auto& self = *static_cast<HostQueue*>(opaque);
        if (self.count == 0) {
            return false;
        }
        const auto job = self.jobs[self.head];
        self.head = (self.head + 1) % self.jobs.size();
        --self.count;
        job.execute(
            job.execution_context,
            job.completion_context,
            job.completion_token,
            self.next_worker++ % 2u);
        return true;
    }
};

struct InstalledHalV2Backend {
    rt::HalV2BackendApi api() noexcept {
        rt::HalV2BackendApi table;
        table.instance = this;
        table.get_capabilities = &get_capabilities;
        table.initialize = &initialize;
        table.register_buffer = &register_buffer;
        table.unregister_buffer = &unregister_buffer;
        table.submit = &submit;
        table.poll = &poll;
        table.cancel = &cancel;
        table.get_health = &get_health;
        table.reset = &reset;
        table.shutdown = &shutdown;
        return table;
    }

    rt::HalV2MemoryTopologyExtension memory_api() noexcept {
      rt::HalV2MemoryTopologyExtension table;
      table.instance = this;
      table.discover = &discover_memory_topology;
      table.register_memory = &register_memory;
      table.unregister_memory = &unregister_memory;
      table.query_timestamp_correlation = &query_timestamp_correlation;
      return table;
    }

    bool initialized = false;
    bool memory_registered = false;

    static InstalledHalV2Backend* self(void* instance) noexcept {
        return static_cast<InstalledHalV2Backend*>(instance);
    }

    static rt::HalV2Status get_capabilities(
        void* instance,
        rt::HalV2Capabilities* output) noexcept {
        if (!self(instance) || !output ||
            output->struct_size < sizeof(*output)) {
            return rt::HalV2Status::invalid_argument;
        }
        *output = {};
        output->struct_size = sizeof(*output);
        output->api_version = rt::hal_v2_api_version;
        output->max_in_flight = 64;
        output->max_registered_buffers = 8;
        output->max_buffer_bytes = 4096;
        output->inline_payload_capacity =
            rt::hal_v2_inline_payload_capacity;
        output->buffer_ref_capacity =
            rt::hal_v2_buffer_ref_capacity;
        output->supports_cancel = 0;
        output->supports_reset = 1;
        output->deterministic_mock = 1;
        constexpr std::string_view identifier =
            "installed.native.hal.v2";
        for (std::size_t index = 0; index < identifier.size(); ++index) {
            output->backend_id[index] = identifier[index];
        }
        return rt::HalV2Status::ok;
    }

    static rt::HalV2Status initialize(
        void* instance,
        const rt::HalV2InitializeConfig* config) noexcept {
        auto* backend = self(instance);
        if (!backend || !config ||
            config->struct_size < sizeof(*config) ||
            config->api_version != rt::hal_v2_api_version ||
            backend->initialized) {
            return rt::HalV2Status::invalid_argument;
        }
        backend->initialized = true;
        return rt::HalV2Status::ok;
    }

    static rt::HalV2Status
    discover_memory_topology(void *instance,
                             rt::HalV2MemoryTopologySnapshot *output) noexcept {
      if (!self(instance) || !output || output->struct_size < sizeof(*output)) {
        return rt::HalV2Status::invalid_argument;
      }
      *output = {};
      output->memory_domain_count = 1;
      output->topology_node_count = 1;
      output->timestamp_domain_count = 1;
      output->completion_timestamp_domain_identity = 1;

      auto &domain = output->memory_domains[0];
      domain.identity = 1;
      domain.kind = static_cast<std::uint32_t>(rt::HalV2MemoryDomainKind::host);
      domain.ownership_modes = rt::hal_v2_memory_ownership_borrowed_host;
      domain.maximum_bytes = 4096;
      domain.byte_granularity = 1;
      domain.alignment = 1;
      domain.offset_granularity = 1;
      domain.access =
          RTFW_DEVICE_BUFFER_HOST_READ | RTFW_DEVICE_BUFFER_HOST_WRITE |
          RTFW_DEVICE_BUFFER_DEVICE_READ | RTFW_DEVICE_BUFFER_DEVICE_WRITE;
      domain.coherency =
          static_cast<std::uint32_t>(rt::HalV2MemoryCoherency::host_coherent);
      domain.topology_node_identity = 1;
      domain.timestamp_domain_identity = 1;

      auto &node = output->topology_nodes[0];
      node.identity = 1;
      node.kind = static_cast<std::uint32_t>(rt::HalV2TopologyNodeKind::host);

      auto &timestamp = output->timestamp_domains[0];
      timestamp.identity = 1;
      timestamp.kind = static_cast<std::uint32_t>(
          rt::HalV2TimestampDomainKind::runtime_monotonic);
      timestamp.tick_numerator_ns = 1;
      timestamp.tick_denominator = 1;
      timestamp.monotonic = 1;
      return rt::HalV2Status::ok;
    }

    static rt::HalV2Status
    register_memory(void *instance,
                    const rt::HalV2MemoryRegistration *registration,
                    rt::HalV2MemoryToken *token) noexcept {
      auto *backend = self(instance);
      if (!backend || !backend->initialized || backend->memory_registered ||
          !registration || !token ||
          registration->struct_size < sizeof(*registration) ||
          registration->extension_version !=
              rt::hal_v2_memory_topology_extension_version ||
          registration->domain_identity != 1 ||
          registration->host_data == nullptr || registration->bytes == 0 ||
          token->struct_size < sizeof(*token)) {
        return rt::HalV2Status::invalid_argument;
      }
      *token = {};
      token->submission_token = 1;
      backend->memory_registered = true;
      return rt::HalV2Status::ok;
    }

    static rt::HalV2Status
    unregister_memory(void *instance,
                      const rt::HalV2MemoryRegistration *registration,
                      const rt::HalV2MemoryToken *token) noexcept {
      auto *backend = self(instance);
      if (!backend || !backend->initialized || !backend->memory_registered ||
          !registration || !token || registration->domain_identity != 1 ||
          token->submission_token != 1) {
        return rt::HalV2Status::invalid_argument;
      }
      backend->memory_registered = false;
      return rt::HalV2Status::ok;
    }

    static rt::HalV2Status
    query_timestamp_correlation(void *,
                                const rt::HalV2TimestampCorrelationQuery *,
                                rt::HalV2TimestampCorrelation *) noexcept {
      return rt::HalV2Status::unsupported;
    }

    static rt::HalV2Status register_buffer(
        void*,
        const rt::HalV2BufferRegistration*,
        std::uint64_t*) noexcept {
        return rt::HalV2Status::unsupported;
    }

    static rt::HalV2Status unregister_buffer(
        void*, std::uint64_t) noexcept {
        return rt::HalV2Status::unsupported;
    }

    static rt::HalV2Status submit(
        void*, const rt::HalV2Submission*) noexcept {
        return rt::HalV2Status::unsupported;
    }

    static rt::HalV2Status poll(
        void* instance,
        rt::HalV2Completion*,
        std::uint64_t,
        std::uint64_t* output_count) noexcept {
        auto* backend = self(instance);
        if (!backend || !backend->initialized || !output_count) {
            return rt::HalV2Status::invalid_state;
        }
        *output_count = 0;
        return rt::HalV2Status::ok;
    }

    static rt::HalV2Status cancel(void*, std::uint64_t) noexcept {
        return rt::HalV2Status::unsupported;
    }

    static rt::HalV2Status get_health(
        void* instance,
        rt::HalV2Health* output) noexcept {
        auto* backend = self(instance);
        if (!backend || !output ||
            output->struct_size < sizeof(*output)) {
            return rt::HalV2Status::invalid_argument;
        }
        *output = {};
        output->struct_size = sizeof(*output);
        output->state = static_cast<std::uint32_t>(
            backend->initialized
                ? rt::HalV2HealthState::healthy
                : rt::HalV2HealthState::shutdown);
        output->last_status =
            static_cast<std::int32_t>(rt::HalV2Status::ok);
        return rt::HalV2Status::ok;
    }

    static rt::HalV2Status reset(void* instance) noexcept {
        auto* backend = self(instance);
        return backend && backend->initialized
            ? rt::HalV2Status::ok
            : rt::HalV2Status::invalid_state;
    }

    static rt::HalV2Status shutdown(void* instance) noexcept {
        auto* backend = self(instance);
        if (!backend || !backend->initialized) {
            return rt::HalV2Status::invalid_state;
        }
        backend->initialized = false;
        return rt::HalV2Status::ok;
    }
};

rt::CallbackResult phase(void* opaque, const rt::CallbackContext&) {
    ++*static_cast<std::size_t*>(opaque);
    return rt::CallbackResult::ok;
}

} // namespace

int main() {
    const rt::CpuMemoryPolicy pre_m15_04_policy{0, {}, 0, {}};
    const rt::CpuMemoryPolicyReport pre_m15_04_report{
        rt::cpu_memory_policy_schema_version, 0, {}, 0, {}};
    const rt::MemoryPlan pre_m16_plan{1024, 512};
    const rt::HalV2CommandTimelineCapabilities command_capabilities{};
    const rt::HalV2CommandTimelineExtension command_extension{};
    const rt::DeviceCommand command{};
    const rt::DeviceCommandBatch command_batch{};
    const rt::HalV2BatchCompletion batch_completion{};
    const rt::DeviceTimelineRegistration timeline_registration{};
    const rt::DeviceBatchPhaseRegistration batch_phase_registration{};
    const rt::DeviceRatePhaseBinding device_rate_binding{};
    const rt::CompiledDeviceRatePhase compiled_device_rate{};
    const rt::DeviceRateAdmissionReport device_rate_admission{};
    const rt::DeviceRateAdmissionDiagnostic device_rate_diagnostic{};
    if (pre_m15_04_policy.accounting_declaration_count != 0 ||
        pre_m15_04_report.accounting_complete ||
        pre_m16_plan.rate_plan_bytes != 0 ||
        command_capabilities.extension_version != 1 ||
        command_extension.extension_version != 1 ||
        command.kind != 0 || command_batch.command_count != 0 ||
        batch_completion.batch_id != 0 ||
        timeline_registration.initial_value != 0 ||
        batch_phase_registration.callback != nullptr ||
        device_rate_binding.completion_budget_ns != 0 ||
        !device_rate_binding.payload_roles.empty() ||
        compiled_device_rate.maximum_in_flight != 0 ||
        device_rate_admission.interval_count != 0 ||
        device_rate_diagnostic.status != rt::Status::ok ||
        rt::hal_v2_command_capacity != 16 ||
        rt::hal_v2_timeline_capacity != 16) {
        return 3;
    }

    HostQueue queue;
    rt::Runtime runtime;
    InstalledHalV2Backend native_hal;
    const auto memory_topology = native_hal.memory_api();
    rt::RuntimeConfig config;
    config.executor_policy = rt::ExecutorPolicy::host_adapter;
    config.worker_count = 2;
    config.executor_queue_capacity = 8;
    config.task_scratch_slots = 8;
    rt::CpuMemoryPolicy accounting;
    accounting.accounting_declaration_count = 1;
    accounting.accounting_declarations[0] = {
        rt::thread_resource_accounting_key(rt::thread_role_frame),
        1,
        8u * 1024u * 1024u,
    };
    std::size_t calls = 0;
    rt::PhaseHandle producer_phase;
    rt::PhaseHandle consumer_phase;
    rt::RateDomainHandle producer_rate;
    rt::RateDomainHandle consumer_rate;
    rt::CrossRateChannelHandle channel;
    rt::DeviceBackendHandle native_backend;
    rt::DeviceMemoryDomainHandle host_domain;
    rt::HalV2MemoryDomain host_domain_descriptor;
    rt::DeviceBufferHandle heterogeneous_buffer;
    alignas(64) std::array<std::byte, 64> heterogeneous_storage{};
    const rt::RateExecutionPolicy additive_active_policy{};
    const rt::RateActionRecord rate_action_record{};
    const rt::RateTelemetryCursor rate_cursor{};
    const std::array initial{std::byte{0x2a}};

    if (runtime.configure(config) != rt::Status::ok ||
        runtime.set_cpu_memory_policy(accounting) != rt::Status::ok ||
        runtime.set_host_executor(
            {&queue, 2, 8, &HostQueue::submit, &HostQueue::try_execute_one}) !=
            rt::Status::ok ||
        runtime.register_device_backend(
            rt::HalV2BackendRegistration{"installed.native.hal",
                                         native_hal.api(), &memory_topology},
            native_backend) != rt::Status::ok ||
        !native_backend.valid() ||
        runtime.device_memory_domain_count(native_backend) != 1 ||
        !runtime.device_memory_domain_at(native_backend, 0, host_domain,
                                         host_domain_descriptor) ||
        !host_domain.valid() ||
        host_domain_descriptor.kind !=
            static_cast<std::uint32_t>(rt::HalV2MemoryDomainKind::host) ||
        runtime.register_device_buffer(
            rt::HeterogeneousDeviceBufferRegistration{
                "installed.host.memory",
                native_backend,
                host_domain,
                heterogeneous_storage,
                {},
                heterogeneous_storage.size(),
                rt::HalV2MemoryOwnership::borrowed_host,
                RTFW_DEVICE_BUFFER_HOST_READ | RTFW_DEVICE_BUFFER_HOST_WRITE |
                    RTFW_DEVICE_BUFFER_DEVICE_READ |
                    RTFW_DEVICE_BUFFER_DEVICE_WRITE,
                rt::HalV2MemoryCoherency::host_coherent,
                rt::hal_v2_memory_sync_none},
            heterogeneous_buffer) != rt::Status::ok ||
        !heterogeneous_buffer.valid() ||
        runtime.register_callback({"producer.phase", &phase, &calls},
                                  producer_phase) != rt::Status::ok ||
        runtime.register_callback({"consumer.phase", &phase, &calls},
                                  consumer_phase) != rt::Status::ok ||
        runtime.register_rate_domain(
            {"producer.rate", 1'000'000, 1, 1'000'000, 0}, producer_rate) !=
            rt::Status::ok ||
        runtime.register_rate_domain(
            {"consumer.rate", 2'000'000, 1, 2'000'000, 0}, consumer_rate) !=
            rt::Status::ok ||
        runtime.bind_phase_to_rate_domain(producer_phase, producer_rate) !=
            rt::Status::ok ||
        runtime.bind_phase_to_rate_domain(consumer_phase, consumer_rate) !=
            rt::Status::ok ||
        runtime.register_cross_rate_channel({"producer.to.consumer",
                                             producer_phase, consumer_phase,
                                             initial.size(), initial},
                                            channel) != rt::Status::ok ||
        runtime.finalize() != rt::Status::ok) {
      return 1;
    }
    rt::CpuMemoryPolicyReport accounting_report;
    rt::CompiledRateDomain compiled_rate;
    rt::CompiledCrossRateChannel compiled_channel;
    rt::DeviceMemoryObjectInfo memory_object;
    std::array<std::byte, 1> copied_initial{};
    if (!runtime.cpu_memory_policy_report(accounting_report) ||
        accounting_report.thread_count == 0 ||
        accounting_report.threads[0].accounting_key.value == 0 ||
        !runtime.compiled_rate_domain_at(0, compiled_rate) ||
        compiled_rate.domain != producer_rate ||
        !runtime.compiled_cross_rate_channel_at(0, compiled_channel) ||
        compiled_channel.channel != channel ||
        runtime.copy_cross_rate_initial_sample(0, copied_initial) !=
            rt::Status::ok ||
        copied_initial != initial ||
        !runtime.device_memory_object_at(0, memory_object) ||
        memory_object.buffer != heterogeneous_buffer ||
        memory_object.domain != host_domain ||
        memory_object.bytes != heterogeneous_storage.size() ||
        memory_object.heterogeneous != 1 ||
        memory_object.host_addressable != 1 ||
        additive_active_policy.maximum_dispatch_records_per_step != 0 ||
        additive_active_policy.host_policy_version != 1 ||
        rate_action_record.schema_version != rt::rate_action_schema_version ||
        rate_action_record.record_size != sizeof(rt::RateActionRecord) ||
        rate_cursor.schema_version != rt::rate_action_schema_version ||
        runtime.rate_execution_enabled() ||
        runtime.reference_release_count() != 3 ||
        runtime.start() != rt::Status::ok ||
        runtime.step({1, std::chrono::nanoseconds(1'000'000), std::nullopt}) !=
            rt::Status::ok ||
        runtime.stop() != rt::Status::ok) {
      return 1;
    }
    return calls == 2 && !native_hal.initialized &&
                   !native_hal.memory_registered
               ? 0
               : 2;
}
