#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

#include <rt/runtime.hpp>

namespace {

template <typename Range> bool all_zero(const Range &values) {
  return std::all_of(values.begin(), values.end(),
                     [](const auto value) { return value == 0; });
}

template <typename Range>
void set_identifier(Range &output, std::string_view value) {
  output.fill(0);
  std::copy(value.begin(), value.end(), output.begin());
}

rt::RuntimeConfig batch_config(std::size_t backend_capacity = 1) {
  rt::RuntimeConfig config;
  config.callback_capacity = 8;
  config.worker_count = 2;
  config.executor_queue_capacity = 8;
  config.task_scratch_slots = 8;
  config.trace_capacity = 32;
  config.device_backend_capacity = backend_capacity;
  config.device_buffer_capacity = 8;
  config.device_outstanding_capacity = 2;
  config.device_completion_batch = 2;
  return config;
}

rt::HalV2MemoryTopologySnapshot batch_snapshot() {
  rt::HalV2MemoryTopologySnapshot snapshot;
  snapshot.memory_domain_count = 2;
  snapshot.topology_node_count = 1;
  snapshot.timestamp_domain_count = 1;
  snapshot.completion_timestamp_domain_identity = 201;

  auto &node = snapshot.topology_nodes[0];
  node.identity = 101;
  node.kind = static_cast<std::uint32_t>(rt::HalV2TopologyNodeKind::host);

  auto &timestamp = snapshot.timestamp_domains[0];
  timestamp.identity = 201;
  timestamp.kind = static_cast<std::uint32_t>(
      rt::HalV2TimestampDomainKind::runtime_monotonic);
  timestamp.tick_numerator_ns = 1;
  timestamp.tick_denominator = 1;
  timestamp.monotonic = 1;

  auto &coherent = snapshot.memory_domains[0];
  coherent.identity = 1;
  coherent.kind = static_cast<std::uint32_t>(rt::HalV2MemoryDomainKind::host);
  coherent.ownership_modes = rt::hal_v2_memory_ownership_borrowed_host;
  coherent.maximum_bytes = 4096;
  coherent.byte_granularity = 1;
  coherent.alignment = 1;
  coherent.offset_granularity = 1;
  coherent.access =
      RTFW_DEVICE_BUFFER_HOST_READ | RTFW_DEVICE_BUFFER_HOST_WRITE |
      RTFW_DEVICE_BUFFER_DEVICE_READ | RTFW_DEVICE_BUFFER_DEVICE_WRITE;
  coherent.coherency =
      static_cast<std::uint32_t>(rt::HalV2MemoryCoherency::host_coherent);
  coherent.topology_node_identity = 101;
  coherent.timestamp_domain_identity = 201;

  auto &explicit_domain = snapshot.memory_domains[1];
  explicit_domain = coherent;
  explicit_domain.identity = 2;
  explicit_domain.kind =
      static_cast<std::uint32_t>(rt::HalV2MemoryDomainKind::pinned_host);
  explicit_domain.coherency = static_cast<std::uint32_t>(
      rt::HalV2MemoryCoherency::explicit_flush_invalidate);
  explicit_domain.required_synchronization =
      rt::hal_v2_memory_sync_flush | rt::hal_v2_memory_sync_invalidate;
  return snapshot;
}

struct BatchBackend {
  enum class SubmitMode : std::uint32_t {
    complete,
    no_completion,
    error,
    throwing,
    blocked,
    malformed_completion,
  };

  BatchBackend() {
    set_identifier(core_capabilities.backend_id, "test.command.v2");
  }

  rt::HalV2BackendApi core_api() {
    rt::HalV2BackendApi api;
    api.instance = this;
    api.get_capabilities = &get_core_capabilities;
    api.initialize = &initialize;
    api.register_buffer = &register_buffer;
    api.unregister_buffer = &unregister_buffer;
    api.submit = &submit_single;
    api.poll = &poll_single;
    api.cancel = &cancel_single;
    api.get_health = &get_health;
    api.reset = &reset;
    api.shutdown = &shutdown;
    return api;
  }

  rt::HalV2MemoryTopologyExtension memory_extension() {
    rt::HalV2MemoryTopologyExtension extension;
    extension.instance = this;
    extension.discover = &discover_memory;
    extension.register_memory = &register_memory;
    extension.unregister_memory = &unregister_memory;
    extension.query_timestamp_correlation = &query_correlation;
    return extension;
  }

  rt::HalV2CommandTimelineExtension command_extension() {
    rt::HalV2CommandTimelineExtension extension;
    extension.instance = this;
    extension.get_capabilities = &get_command_capabilities;
    extension.submit = &submit_batch;
    extension.poll = &poll_batches;
    extension.cancel = &cancel_batch;
    extension.request_stop = &request_stop;
    return extension;
  }

  rt::HalV2Capabilities core_capabilities = [] {
    rt::HalV2Capabilities value;
    value.max_in_flight = 8;
    value.max_registered_buffers = 8;
    value.max_buffer_bytes = 4096;
    value.supports_cancel = 1;
    value.supports_reset = 1;
    value.deterministic_mock = 1;
    return value;
  }();
  rt::HalV2CommandTimelineCapabilities command_capabilities = [] {
    rt::HalV2CommandTimelineCapabilities value;
    value.max_in_flight_batches = 2;
    value.max_commands_per_batch = rt::hal_v2_command_capacity;
    value.max_wait_points = rt::hal_v2_timeline_wait_capacity;
    value.max_signal_points = rt::hal_v2_timeline_signal_capacity;
    value.max_timelines = rt::hal_v2_timeline_capacity;
    value.completion_batch_capacity = 2;
    value.backend_control_storage_bytes = 256;
    return value;
  }();
  rt::HalV2MemoryTopologySnapshot snapshot = batch_snapshot();
  std::atomic<SubmitMode> mode{SubmitMode::complete};
  std::atomic<rt::HalV2Status> capabilities_status{rt::HalV2Status::ok};
  std::atomic<bool> leave_capabilities_untouched{false};
  std::atomic<bool> partially_write_capabilities{false};
  std::atomic<bool> throw_capabilities{false};
  std::atomic<std::size_t> capabilities_calls{0};
  std::atomic<bool> incoming_capabilities_header_exact{false};
  std::atomic<bool> incoming_capabilities_semantics_zero{false};
  std::atomic<bool> submit_entered{false};
  std::atomic<bool> release_submit{false};
  std::atomic<std::uint32_t> completion_ready{0};
  std::atomic<std::size_t> batch_submit_calls{0};
  std::atomic<std::size_t> batch_poll_calls{0};
  std::atomic<std::size_t> batch_cancel_calls{0};
  std::atomic<std::size_t> stop_request_calls{0};
  std::atomic<std::size_t> single_submit_calls{0};
  std::atomic<std::size_t> memory_registration_calls{0};
  rt::DeviceCommandBatch last_batch{};
  rt::HalV2BatchCompletion pending_completion{};

  static BatchBackend *self(void *instance) {
    return static_cast<BatchBackend *>(instance);
  }

  static rt::HalV2Status get_core_capabilities(void *instance,
                                               rt::HalV2Capabilities *output) {
    if (!instance || !output) {
      return rt::HalV2Status::invalid_argument;
    }
    *output = self(instance)->core_capabilities;
    return rt::HalV2Status::ok;
  }

  static rt::HalV2Status initialize(void *instance,
                                    const rt::HalV2InitializeConfig *) {
    return instance ? rt::HalV2Status::ok : rt::HalV2Status::invalid_argument;
  }

  static rt::HalV2Status register_buffer(void *instance,
                                         const rt::HalV2BufferRegistration *,
                                         std::uint64_t *token) {
    if (!instance || !token) {
      return rt::HalV2Status::invalid_argument;
    }
    *token = 0x1001;
    return rt::HalV2Status::ok;
  }

  static rt::HalV2Status unregister_buffer(void *instance, std::uint64_t) {
    return instance ? rt::HalV2Status::ok : rt::HalV2Status::invalid_argument;
  }

  static rt::HalV2Status submit_single(void *instance,
                                       const rt::HalV2Submission *) {
    if (!instance) {
      return rt::HalV2Status::invalid_argument;
    }
    self(instance)->single_submit_calls.fetch_add(1);
    return rt::HalV2Status::ok;
  }

  static rt::HalV2Status poll_single(void *instance, rt::HalV2Completion *,
                                     std::uint64_t, std::uint64_t *count) {
    if (!instance || !count) {
      return rt::HalV2Status::invalid_argument;
    }
    *count = 0;
    return rt::HalV2Status::ok;
  }

  static rt::HalV2Status cancel_single(void *, std::uint64_t) {
    return rt::HalV2Status::unsupported;
  }

  static rt::HalV2Status get_health(void *instance, rt::HalV2Health *health) {
    if (!instance || !health) {
      return rt::HalV2Status::invalid_argument;
    }
    *health = {};
    health->state = static_cast<std::uint32_t>(rt::HalV2HealthState::healthy);
    return rt::HalV2Status::ok;
  }

  static rt::HalV2Status reset(void *instance) {
    return instance ? rt::HalV2Status::ok : rt::HalV2Status::invalid_argument;
  }

  static rt::HalV2Status shutdown(void *instance) {
    return instance ? rt::HalV2Status::ok : rt::HalV2Status::invalid_argument;
  }

  static rt::HalV2Status
  discover_memory(void *instance, rt::HalV2MemoryTopologySnapshot *output) {
    if (!instance || !output) {
      return rt::HalV2Status::invalid_argument;
    }
    *output = self(instance)->snapshot;
    return rt::HalV2Status::ok;
  }

  static rt::HalV2Status register_memory(void *instance,
                                         const rt::HalV2MemoryRegistration *,
                                         rt::HalV2MemoryToken *token) {
    if (!instance || !token) {
      return rt::HalV2Status::invalid_argument;
    }
    auto &backend = *self(instance);
    const auto ordinal = backend.memory_registration_calls.fetch_add(1) + 1;
    token->submission_token = 0x2000 + ordinal;
    token->native_token.size = 1;
    token->native_token.bytes[0] = std::byte{0x42};
    return rt::HalV2Status::ok;
  }

  static rt::HalV2Status unregister_memory(void *instance,
                                           const rt::HalV2MemoryRegistration *,
                                           const rt::HalV2MemoryToken *) {
    return instance ? rt::HalV2Status::ok : rt::HalV2Status::invalid_argument;
  }

  static rt::HalV2Status
  query_correlation(void *, const rt::HalV2TimestampCorrelationQuery *,
                    rt::HalV2TimestampCorrelation *) {
    return rt::HalV2Status::unsupported;
  }

  static rt::HalV2Status
  get_command_capabilities(void *instance,
                           rt::HalV2CommandTimelineCapabilities *output) {
    if (!instance || !output) {
      return rt::HalV2Status::invalid_argument;
    }
    auto &backend = *self(instance);
    backend.capabilities_calls.fetch_add(1, std::memory_order_relaxed);
    backend.incoming_capabilities_header_exact.store(
        output->struct_size == sizeof(*output) &&
            output->extension_version ==
                rt::hal_v2_command_timeline_extension_version,
        std::memory_order_relaxed);
    backend.incoming_capabilities_semantics_zero.store(
        output->max_in_flight_batches == 0 &&
            output->max_commands_per_batch == 0 &&
            output->max_wait_points == 0 &&
            output->max_signal_points == 0 && output->max_timelines == 0 &&
            output->completion_batch_capacity == 0 &&
            output->backend_control_storage_bytes == 0 &&
            all_zero(output->reserved),
        std::memory_order_relaxed);
    if (backend.throw_capabilities.load()) {
      throw std::runtime_error("command capabilities");
    }
    const auto status = backend.capabilities_status.load();
    if (status != rt::HalV2Status::ok) {
      return status;
    }
    if (backend.partially_write_capabilities.load()) {
      output->max_in_flight_batches =
          backend.command_capabilities.max_in_flight_batches;
      return rt::HalV2Status::ok;
    }
    if (!backend.leave_capabilities_untouched.load()) {
      *output = backend.command_capabilities;
    }
    return rt::HalV2Status::ok;
  }

  static rt::HalV2Status submit_batch(void *instance,
                                      const rt::DeviceCommandBatch *batch) {
    if (!instance || !batch) {
      return rt::HalV2Status::invalid_argument;
    }
    auto &backend = *self(instance);
    backend.last_batch = *batch;
    backend.batch_submit_calls.fetch_add(1);
    const auto behavior = backend.mode.load();
    if (behavior == SubmitMode::throwing) {
      throw std::runtime_error("batch submit");
    }
    if (behavior == SubmitMode::error) {
      return rt::HalV2Status::error;
    }
    if (behavior == SubmitMode::blocked) {
      backend.submit_entered.store(true, std::memory_order_release);
      backend.submit_entered.notify_all();
      while (!backend.release_submit.load(std::memory_order_acquire)) {
        backend.release_submit.wait(false, std::memory_order_relaxed);
      }
      return rt::HalV2Status::canceled;
    }
    if (behavior == SubmitMode::no_completion) {
      return rt::HalV2Status::ok;
    }
    backend.pending_completion = {};
    backend.pending_completion.batch_id = batch->batch_id;
    backend.pending_completion.signal_count = batch->signal_count;
    backend.pending_completion.device_timestamp = 77;
    backend.pending_completion.timestamp_domain_identity = 201;
    for (std::size_t index = 0; index < batch->signal_count; ++index) {
      backend.pending_completion.signals[index] = batch->signals[index];
    }
    if (behavior == SubmitMode::malformed_completion) {
      backend.pending_completion.timestamp_domain_identity = 999;
    }
    backend.completion_ready.store(1, std::memory_order_release);
    return rt::HalV2Status::ok;
  }

  static rt::HalV2Status poll_batches(void *instance,
                                      rt::HalV2BatchCompletion *completions,
                                      std::uint64_t capacity,
                                      std::uint64_t *count) {
    if (!instance || !count || (capacity != 0 && !completions)) {
      return rt::HalV2Status::invalid_argument;
    }
    auto &backend = *self(instance);
    backend.batch_poll_calls.fetch_add(1);
    if (capacity != 0 &&
        backend.completion_ready.exchange(0, std::memory_order_acq_rel) != 0) {
      completions[0] = backend.pending_completion;
      *count = 1;
    } else {
      *count = 0;
    }
    return rt::HalV2Status::ok;
  }

  static rt::HalV2Status cancel_batch(void *instance, std::uint64_t) {
    if (!instance) {
      return rt::HalV2Status::invalid_argument;
    }
    self(instance)->batch_cancel_calls.fetch_add(1);
    return rt::HalV2Status::ok;
  }

  static rt::HalV2Status request_stop(void *instance) {
    if (!instance) {
      return rt::HalV2Status::invalid_argument;
    }
    auto &backend = *self(instance);
    backend.stop_request_calls.fetch_add(1);
    backend.release_submit.store(true, std::memory_order_release);
    backend.release_submit.notify_all();
    return rt::HalV2Status::ok;
  }
};

rt::Status register_backend(rt::Runtime &runtime, BatchBackend &backend,
                            std::string_view name,
                            rt::DeviceBackendHandle &handle,
                            bool with_commands = true) {
  auto memory = backend.memory_extension();
  auto command = backend.command_extension();
  return runtime.register_device_backend(
      {name, backend.core_api(), &memory, with_commands ? &command : nullptr},
      handle);
}

rt::HalV2BufferReference reference(rt::DeviceBufferHandle buffer,
                                   std::uint32_t access,
                                   std::uint64_t bytes = 16) {
  rt::HalV2BufferReference result;
  result.buffer_token = buffer.value;
  result.access = access;
  result.bytes = bytes;
  return result;
}

rt::DeviceCommandBatch dispatch_declaration(rt::DeviceBufferHandle buffer,
                                            rt::DeviceTimelineHandle timeline) {
  rt::DeviceCommandBatch declaration;
  declaration.command_count = 1;
  declaration.signal_count = 1;
  auto &command = declaration.commands[0];
  command.kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
  command.opcode = 7;
  command.buffer_count = 1;
  command.buffers[0] = reference(buffer, RTFW_DEVICE_ACCESS_READ);
  declaration.signals[0].timeline_handle = timeline.value;
  return declaration;
}

struct BatchProvider {
  rt::DeviceCommandBatch declaration{};
  std::uint64_t wait_value = 0;
  std::uint64_t signal_value = 1;
  std::uint64_t timeout_ns = 100'000'000;
  bool malformed_timeout = false;
  std::atomic<std::size_t> *callback_count = nullptr;
};

rt::CallbackResult provide_batch(void *user_data,
                                 const rt::DeviceCallbackContext &,
                                 rt::DeviceCommandBatch &batch) {
  auto &provider = *static_cast<BatchProvider *>(user_data);
  if (provider.callback_count) {
    provider.callback_count->fetch_add(1, std::memory_order_release);
    provider.callback_count->notify_all();
  }
  batch = provider.declaration;
  batch.timeout_ns = provider.malformed_timeout ? 0 : provider.timeout_ns;
  for (std::size_t index = 0; index < batch.wait_count; ++index) {
    batch.waits[index].value = provider.wait_value;
  }
  for (std::size_t index = 0; index < batch.signal_count; ++index) {
    batch.signals[index].value = provider.signal_value + index;
  }
  return rt::CallbackResult::ok;
}

struct ConfiguredBatchRuntime {
  BatchBackend backend;
  rt::Runtime runtime;
  rt::DeviceBackendHandle backend_handle{};
  rt::DeviceBufferHandle buffer{};
  rt::DeviceTimelineHandle timeline{};
  std::array<std::byte, 64> storage{};

    void configure(std::size_t worker_count = 2) {
        auto config = batch_config();
        config.worker_count = worker_count;
        ASSERT_EQ(runtime.configure(config), rt::Status::ok);
    ASSERT_EQ(register_backend(runtime, backend, "command", backend_handle),
              rt::Status::ok);
    ASSERT_EQ(runtime.register_device_buffer(
                  {"command.buffer", backend_handle, storage}, buffer),
              rt::Status::ok);
    ASSERT_EQ(runtime.register_device_timeline(
                  {"command.timeline", backend_handle, 0}, timeline),
              rt::Status::ok);
  }
};

std::uint64_t device_rate_graph_id(
    bool reverse_binding_order,
    std::uint64_t second_completion_budget = 4) {
  ConfiguredBatchRuntime fixture;
  fixture.configure();
  if (fixture.runtime.set_rate_execution_policy({4}) != rt::Status::ok) {
    ADD_FAILURE() << fixture.runtime.last_error();
    return 0;
  }
  BatchProvider first;
  BatchProvider second;
  rt::DeviceTimelineHandle second_timeline;
  if (fixture.runtime.register_device_timeline(
          {"command.timeline.second", fixture.backend_handle, 0},
          second_timeline) != rt::Status::ok) {
    ADD_FAILURE() << fixture.runtime.last_error();
    return 0;
  }
  first.declaration = dispatch_declaration(fixture.buffer, fixture.timeline);
  second.declaration = dispatch_declaration(fixture.buffer, second_timeline);
  rt::PhaseHandle first_phase;
  rt::PhaseHandle second_phase;
  if (fixture.runtime.register_device_batch_phase(
          {"rate.first", fixture.backend_handle, &provide_batch,
           &first, first.declaration}, first_phase) != rt::Status::ok ||
      fixture.runtime.register_device_batch_phase(
          {"rate.second", fixture.backend_handle, &provide_batch,
           &second, second.declaration}, second_phase) != rt::Status::ok) {
    ADD_FAILURE() << fixture.runtime.last_error();
    return 0;
  }
  rt::RateDomainHandle domain;
  if (fixture.runtime.register_rate_domain(
          {"device.rate.identity", 10, 1, 6, 0,
           rt::RateCriticality::critical, false,
           rt::RateLateAction::fail, 0}, domain) != rt::Status::ok) {
    ADD_FAILURE() << fixture.runtime.last_error();
    return 0;
  }
  const std::array roles{rt::DeviceRatePayloadRole::input};
  const rt::DeviceRatePhaseBinding first_binding{
      first_phase, domain, 4, 1, roles};
  const rt::DeviceRatePhaseBinding second_binding{
      second_phase, domain, second_completion_budget, 1, roles};
  const auto first_status = reverse_binding_order
      ? fixture.runtime.bind_device_phase_to_rate_domain(second_binding)
      : fixture.runtime.bind_device_phase_to_rate_domain(first_binding);
  const auto second_status = reverse_binding_order
      ? fixture.runtime.bind_device_phase_to_rate_domain(first_binding)
      : fixture.runtime.bind_device_phase_to_rate_domain(second_binding);
  if (first_status != rt::Status::ok || second_status != rt::Status::ok ||
      fixture.runtime.finalize() != rt::Status::ok) {
    ADD_FAILURE() << fixture.runtime.last_error();
    return 0;
  }
  std::size_t required = 0;
  if (fixture.runtime.checkpoint_size(required) != rt::Status::ok) {
    ADD_FAILURE() << fixture.runtime.last_error();
    return 0;
  }
  std::vector<std::byte> checkpoint(required);
  rt::ArtifactWriteResult result;
  rt::CheckpointMetadata metadata;
  if (fixture.runtime.write_checkpoint(0, checkpoint, result) !=
          rt::Status::ok ||
      rt::inspect_checkpoint_artifact(checkpoint, metadata) != rt::Status::ok) {
    ADD_FAILURE() << fixture.runtime.last_error();
    return 0;
  }
  return metadata.graph_id;
}

const rt::ThreadPolicyReport *
find_thread(const rt::CpuMemoryPolicyReport &report, rt::ThreadRoleId role) {
  const auto end =
      report.threads.begin() + static_cast<std::ptrdiff_t>(report.thread_count);
  const auto found =
      std::find_if(report.threads.begin(), end,
                   [role](const auto &row) { return row.role == role; });
  return found == end ? nullptr : &*found;
}

} // namespace

TEST(CommandBatch, PublicConstantsLayoutsDefaultsAndAggregatePrefixAreExact) {
  EXPECT_EQ(rt::hal_v2_command_timeline_extension_version, 1u);
  EXPECT_EQ(rt::hal_v2_command_capacity, 16u);
  EXPECT_EQ(rt::hal_v2_timeline_wait_capacity, 8u);
  EXPECT_EQ(rt::hal_v2_timeline_signal_capacity, 8u);
  EXPECT_EQ(rt::hal_v2_timeline_capacity, 16u);
  EXPECT_EQ(static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch), 1u);
  EXPECT_EQ(static_cast<std::uint32_t>(rt::HalV2CommandKind::copy), 2u);
  EXPECT_EQ(
      static_cast<std::uint32_t>(rt::HalV2CommandKind::memory_synchronization),
      3u);
  EXPECT_EQ(sizeof(rt::DeviceTimelineHandle), 8u);
  EXPECT_EQ(sizeof(rt::HalV2TimelinePoint), 48u);
  EXPECT_EQ(sizeof(rt::DeviceCommand), 544u);
  EXPECT_EQ(sizeof(rt::DeviceCommandBatch), 9584u);
  EXPECT_EQ(sizeof(rt::HalV2BatchCompletion), 488u);
  EXPECT_EQ(sizeof(rt::HalV2CommandTimelineCapabilities), 104u);
  EXPECT_EQ(sizeof(rt::HalV2CommandTimelineExtension), 120u);
  EXPECT_EQ(sizeof(rt::DeviceTimelineInfo), 144u);
  EXPECT_EQ(sizeof(rt::HalV2BackendRegistration), 192u);
  static_assert(std::is_standard_layout_v<rt::DeviceCommand>);
  static_assert(std::is_standard_layout_v<rt::DeviceCommandBatch>);
  static_assert(std::is_standard_layout_v<rt::HalV2BatchCompletion>);

  const rt::DeviceCommand command;
  const rt::DeviceCommandBatch batch;
  const rt::HalV2BatchCompletion completion;
  const rt::HalV2CommandTimelineCapabilities capabilities;
  const rt::HalV2CommandTimelineExtension extension;
  EXPECT_EQ(command.kind, 0u);
  EXPECT_EQ(batch.command_count, 0u);
  EXPECT_EQ(completion.batch_id, 0u);
  EXPECT_EQ(capabilities.max_in_flight_batches, 0u);
  EXPECT_EQ(extension.submit, nullptr);
  EXPECT_TRUE(all_zero(command.reserved));
  EXPECT_TRUE(all_zero(batch.reserved));
  EXPECT_TRUE(all_zero(completion.reserved));
  EXPECT_TRUE(all_zero(capabilities.reserved));
  EXPECT_TRUE(all_zero(extension.reserved));

  const rt::DeviceRatePhaseBinding device_rate{};
  const rt::CompiledDeviceRatePhase compiled_device_rate{};
  const rt::DeviceRateAdmissionReport admission{};
  const rt::DeviceRateAdmissionDiagnostic admission_diagnostic{};
  EXPECT_FALSE(device_rate.phase.valid());
  EXPECT_EQ(device_rate.completion_budget_ns, 0u);
  EXPECT_EQ(device_rate.maximum_in_flight, 0u);
  EXPECT_TRUE(device_rate.payload_roles.empty());
  EXPECT_FALSE(compiled_device_rate.phase.valid());
  EXPECT_EQ(admission.interval_count, 0u);
  EXPECT_EQ(admission_diagnostic.status, rt::Status::ok);

  const rt::HalV2BackendRegistration old_prefix{"old", {}};
  EXPECT_EQ(old_prefix.memory_topology, nullptr);
  EXPECT_EQ(old_prefix.command_timeline, nullptr);
}

TEST(CommandBatch, DeviceRateModelCompilesInspectsAccountsAndRejectsStart) {
  ConfiguredBatchRuntime fixture;
  fixture.configure();
  ASSERT_EQ(fixture.runtime.set_rate_execution_policy({4}), rt::Status::ok);
  BatchProvider provider;
  provider.declaration = dispatch_declaration(fixture.buffer, fixture.timeline);
  rt::PhaseHandle phase;
  ASSERT_EQ(fixture.runtime.register_device_batch_phase(
                {"rate.batch", fixture.backend_handle, &provide_batch,
                 &provider, provider.declaration},
                phase),
            rt::Status::ok);
  rt::RateDomainHandle domain;
  ASSERT_EQ(fixture.runtime.register_rate_domain(
                {"device.rate", 10, 1, 6, 0,
                 rt::RateCriticality::critical, false,
                 rt::RateLateAction::fail, 0},
                domain),
            rt::Status::ok);
  const std::array roles{rt::DeviceRatePayloadRole::input};
  ASSERT_EQ(fixture.runtime.bind_device_phase_to_rate_domain(
                {phase, domain, 4, 1, roles}),
            rt::Status::ok);
  ASSERT_EQ(fixture.runtime.finalize(), rt::Status::ok)
      << fixture.runtime.last_error();

  EXPECT_TRUE(fixture.runtime.device_rate_model_enabled());
  EXPECT_EQ(fixture.runtime.device_rate_phase_count(), 1u);
  EXPECT_EQ(fixture.runtime.device_rate_command_count(), 1u);
  EXPECT_EQ(fixture.runtime.device_rate_payload_reference_count(), 1u);
  EXPECT_EQ(fixture.runtime.device_rate_timeline_reference_count(), 1u);
  rt::CompiledDeviceRatePhase compiled;
  ASSERT_TRUE(fixture.runtime.compiled_device_rate_phase_at(0, compiled));
  EXPECT_EQ(compiled.phase, phase);
  EXPECT_EQ(compiled.domain, domain);
  EXPECT_EQ(compiled.backend, fixture.backend_handle);
  EXPECT_EQ(compiled.completion_budget_ns, 4u);
  EXPECT_EQ(compiled.maximum_in_flight, 1u);
  EXPECT_EQ(compiled.completion_timestamp_domain_identity, 201u);
  rt::CompiledDeviceRateCommand command;
  ASSERT_TRUE(fixture.runtime.compiled_device_rate_command_at(0, command));
  EXPECT_EQ(command.kind, rt::HalV2CommandKind::dispatch);
  EXPECT_EQ(command.opcode, 7u);
  rt::CompiledDeviceRatePayloadReference payload;
  ASSERT_TRUE(fixture.runtime.compiled_device_rate_payload_reference_at(
      0, payload));
  EXPECT_EQ(payload.buffer, fixture.buffer);
  EXPECT_EQ(payload.role, rt::DeviceRatePayloadRole::input);
  EXPECT_EQ(payload.bytes, 16u);
  rt::CompiledDeviceRateTimelineReference timeline;
  ASSERT_TRUE(fixture.runtime.compiled_device_rate_timeline_reference_at(
      0, timeline));
  EXPECT_EQ(timeline.timeline, fixture.timeline);
  EXPECT_EQ(timeline.role, rt::DeviceRateTimelineRole::signal);
  rt::DeviceRateAdmissionReport admission;
  ASSERT_TRUE(fixture.runtime.device_rate_admission_report(admission));
  EXPECT_EQ(admission.supercycle_ns, 10u);
  EXPECT_EQ(admission.peak_global_in_flight, 1u);
  EXPECT_EQ(admission.interval_count, 1u);
  rt::DeviceRateAdmissionInterval interval;
  ASSERT_TRUE(fixture.runtime.device_rate_admission_interval_at(0, interval));
  EXPECT_EQ(interval.release_time_ns, 0u);
  EXPECT_EQ(interval.completion_deadline_ns, 4u);

  rt::MemoryPlan memory;
  ASSERT_TRUE(fixture.runtime.memory_plan(memory));
  EXPECT_EQ(memory.device_rate_phase_count, 1u);
  EXPECT_EQ(memory.device_rate_command_count, 1u);
  EXPECT_EQ(memory.device_rate_payload_reference_count, 1u);
  EXPECT_EQ(memory.device_rate_timeline_reference_count, 1u);
  EXPECT_EQ(memory.device_rate_admission_interval_count, 1u);
  EXPECT_GT(memory.device_rate_plan_bytes, 0u);

  EXPECT_EQ(fixture.runtime.start(), rt::Status::invalid_state);
  EXPECT_EQ(fixture.runtime.last_error(),
            "active mixed CPU/device execution is deferred until M21-02");
  EXPECT_EQ(fixture.backend.batch_submit_calls.load(), 0u);
  EXPECT_EQ(fixture.backend.batch_poll_calls.load(), 0u);
  EXPECT_EQ(fixture.backend.single_submit_calls.load(), 0u);
  EXPECT_EQ(fixture.runtime.stop(), rt::Status::ok);
}

TEST(CommandBatch, DeviceRateBindingRejectsLegacyAndSupportsCorrection) {
  ConfiguredBatchRuntime fixture;
  fixture.configure();
  BatchProvider provider;
  provider.declaration = dispatch_declaration(fixture.buffer, fixture.timeline);
  rt::PhaseHandle phase;
  ASSERT_EQ(fixture.runtime.register_device_batch_phase(
                {"rate.retry", fixture.backend_handle, &provide_batch,
                 &provider, provider.declaration},
                phase),
            rt::Status::ok);
  rt::RateDomainHandle domain;
  ASSERT_EQ(fixture.runtime.register_rate_domain(
                {"rate.retry.domain", 10, 1, 6, 0,
                 rt::RateCriticality::normal, false},
                domain),
            rt::Status::ok);
  const std::array wrong{rt::DeviceRatePayloadRole::output};
  ASSERT_EQ(fixture.runtime.bind_device_phase_to_rate_domain(
                {phase, domain, 4, 1, wrong}),
            rt::Status::ok);
  EXPECT_EQ(fixture.runtime.finalize(), rt::Status::invalid_config);
  rt::DeviceRateAdmissionDiagnostic diagnostic;
  ASSERT_TRUE(fixture.runtime.device_rate_admission_diagnostic(diagnostic));
  EXPECT_EQ(diagnostic.status, rt::Status::invalid_config);
  EXPECT_EQ(diagnostic.phase, phase);
  EXPECT_EQ(diagnostic.reference_index,
            std::numeric_limits<std::size_t>::max());
  const std::array corrected{rt::DeviceRatePayloadRole::input};
  ASSERT_EQ(fixture.runtime.replace_device_rate_binding(
                {phase, domain, 4, 1, corrected}),
            rt::Status::ok);
  ASSERT_EQ(fixture.runtime.finalize(), rt::Status::ok)
      << fixture.runtime.last_error();
  EXPECT_FALSE(fixture.runtime.device_rate_admission_diagnostic(diagnostic));
  EXPECT_EQ(fixture.runtime.stop(), rt::Status::ok);

  rt::Runtime legacy;
  ASSERT_EQ(legacy.configure(batch_config()), rt::Status::ok);
  rt::PhaseHandle cpu;
  std::size_t calls = 0;
  ASSERT_EQ(legacy.register_callback({"cpu", [](void* data, const auto&) {
                    ++*static_cast<std::size_t*>(data);
                    return rt::CallbackResult::ok;
                  }, &calls}, cpu),
            rt::Status::ok);
  rt::RateDomainHandle legacy_domain;
  ASSERT_EQ(legacy.register_rate_domain(
                {"legacy.rate", 10, 1, 10, 1,
                 rt::RateCriticality::normal, false},
                legacy_domain),
            rt::Status::ok);
  EXPECT_EQ(legacy.bind_device_phase_to_rate_domain(
                {cpu, legacy_domain, 1, 1, {}}),
            rt::Status::invalid_argument);
}

TEST(CommandBatch, DeviceRateIdentityIsCanonicalAndIncludesAdmissionPolicy) {
  const auto forward = device_rate_graph_id(false);
  const auto reverse = device_rate_graph_id(true);
  const auto changed_policy = device_rate_graph_id(false, 5);
  ASSERT_NE(forward, 0u);
  EXPECT_EQ(forward, reverse);
  EXPECT_NE(forward, changed_policy);
}

TEST(CommandBatch, ExtensionDiscoveryRejectsMalformedAndPartialResults) {
  BatchBackend backend;
  auto register_with = [&](rt::HalV2CommandTimelineExtension command) {
    rt::Runtime runtime;
    EXPECT_EQ(runtime.configure(batch_config()), rt::Status::ok);
    auto memory = backend.memory_extension();
    rt::DeviceBackendHandle handle;
    return runtime.register_device_backend(
        {"malformed", backend.core_api(), &memory, &command}, handle);
  };

  auto command = backend.command_extension();
  command.struct_size = sizeof(command) - 1;
  EXPECT_EQ(register_with(command), rt::Status::invalid_argument);
  command = backend.command_extension();
  command.extension_version = 2;
  EXPECT_EQ(register_with(command), rt::Status::invalid_argument);
  command = backend.command_extension();
  command.poll = nullptr;
  EXPECT_EQ(register_with(command), rt::Status::invalid_argument);
  command = backend.command_extension();
  command.reserved[0] = 1;
  EXPECT_EQ(register_with(command), rt::Status::invalid_argument);

  backend.command_capabilities.max_commands_per_batch = 0;
  EXPECT_EQ(register_with(backend.command_extension()),
            rt::Status::invalid_argument);
  backend.command_capabilities.max_commands_per_batch =
      rt::hal_v2_command_capacity;
  backend.command_capabilities.max_in_flight_batches = 1;
  EXPECT_EQ(register_with(backend.command_extension()),
            rt::Status::capacity_exceeded);
  backend.command_capabilities.max_in_flight_batches = 2;
  backend.leave_capabilities_untouched.store(true);
  EXPECT_EQ(register_with(backend.command_extension()),
            rt::Status::invalid_argument);
  backend.leave_capabilities_untouched.store(false);
  backend.throw_capabilities.store(true);
  EXPECT_EQ(register_with(backend.command_extension()),
            rt::Status::device_error);
}

TEST(CommandBatch,
     CapabilityDiscoverySeedsExactHeaderRejectsMalformedAndRetriesCleanly) {
  const auto reject_then_correct = [](auto mutate, rt::Status expected,
                                      std::size_t rejected_callback_calls) {
    BatchBackend backend;
    const auto valid_capabilities = backend.command_capabilities;
    rt::Runtime runtime;
    ASSERT_EQ(runtime.configure(batch_config()), rt::Status::ok);
    auto memory = backend.memory_extension();
    auto command = backend.command_extension();
    mutate(command, backend);

    rt::DeviceBackendHandle rejected;
    EXPECT_EQ(runtime.register_device_backend(
                  {"capability.retry", backend.core_api(), &memory, &command},
                  rejected),
              expected);
    EXPECT_FALSE(rejected.valid());
    EXPECT_EQ(runtime.device_backend_count(), 0u);
    EXPECT_EQ(backend.capabilities_calls.load(), rejected_callback_calls);
    if (rejected_callback_calls != 0) {
      EXPECT_TRUE(backend.incoming_capabilities_header_exact.load());
      EXPECT_TRUE(backend.incoming_capabilities_semantics_zero.load());
    }

    backend.command_capabilities = valid_capabilities;
    backend.capabilities_status.store(rt::HalV2Status::ok);
    backend.leave_capabilities_untouched.store(false);
    backend.partially_write_capabilities.store(false);
    backend.throw_capabilities.store(false);
    command = backend.command_extension();
    rt::DeviceBackendHandle accepted;
    ASSERT_EQ(runtime.register_device_backend(
                  {"capability.retry", backend.core_api(), &memory, &command},
                  accepted),
              rt::Status::ok);
    EXPECT_TRUE(accepted.valid());
    EXPECT_EQ(runtime.device_backend_count(), 1u);
    EXPECT_EQ(backend.capabilities_calls.load(), rejected_callback_calls + 1);
    EXPECT_TRUE(backend.incoming_capabilities_header_exact.load());
    EXPECT_TRUE(backend.incoming_capabilities_semantics_zero.load());
  };

  reject_then_correct(
      [](auto &command, auto &) { command.instance = nullptr; },
      rt::Status::invalid_argument, 0);
  reject_then_correct(
      [](auto &command, auto &) { command.get_capabilities = nullptr; },
      rt::Status::invalid_argument, 0);
  reject_then_correct(
      [](auto &, auto &backend) { backend.throw_capabilities.store(true); },
      rt::Status::device_error, 1);
  reject_then_correct(
      [](auto &, auto &backend) {
        backend.capabilities_status.store(rt::HalV2Status::error);
      },
      rt::Status::device_error, 1);
  reject_then_correct(
      [](auto &, auto &backend) {
        backend.capabilities_status.store(rt::HalV2Status::unsupported);
      },
      rt::Status::device_error, 1);
  reject_then_correct(
      [](auto &, auto &backend) {
        backend.capabilities_status.store(
            rt::HalV2Status::resource_exhausted);
      },
      rt::Status::resource_exhausted, 1);
  reject_then_correct(
      [](auto &, auto &backend) {
        backend.leave_capabilities_untouched.store(true);
      },
      rt::Status::invalid_argument, 1);
  reject_then_correct(
      [](auto &, auto &backend) {
        backend.partially_write_capabilities.store(true);
      },
      rt::Status::invalid_argument, 1);
  reject_then_correct(
      [](auto &, auto &backend) {
        backend.command_capabilities.struct_size =
            sizeof(rt::HalV2CommandTimelineCapabilities) - 1;
      },
      rt::Status::invalid_argument, 1);
  reject_then_correct(
      [](auto &, auto &backend) {
        backend.command_capabilities.extension_version =
            rt::hal_v2_command_timeline_extension_version + 1;
      },
      rt::Status::invalid_argument, 1);
  reject_then_correct(
      [](auto &, auto &backend) {
        backend.command_capabilities.max_in_flight_batches = 0;
      },
      rt::Status::invalid_argument, 1);
  reject_then_correct(
      [](auto &, auto &backend) {
        backend.command_capabilities.max_commands_per_batch = 0;
      },
      rt::Status::invalid_argument, 1);
  reject_then_correct(
      [](auto &, auto &backend) {
        backend.command_capabilities.max_wait_points = 0;
      },
      rt::Status::invalid_argument, 1);
  reject_then_correct(
      [](auto &, auto &backend) {
        backend.command_capabilities.max_signal_points = 0;
      },
      rt::Status::invalid_argument, 1);
  reject_then_correct(
      [](auto &, auto &backend) {
        backend.command_capabilities.max_timelines = 0;
      },
      rt::Status::invalid_argument, 1);
  reject_then_correct(
      [](auto &, auto &backend) {
        backend.command_capabilities.completion_batch_capacity = 0;
      },
      rt::Status::invalid_argument, 1);
  reject_then_correct(
      [](auto &, auto &backend) {
        backend.command_capabilities.max_commands_per_batch =
            rt::hal_v2_command_capacity + 1;
      },
      rt::Status::invalid_argument, 1);
  reject_then_correct(
      [](auto &, auto &backend) {
        backend.command_capabilities.max_wait_points =
            rt::hal_v2_timeline_wait_capacity + 1;
      },
      rt::Status::invalid_argument, 1);
  reject_then_correct(
      [](auto &, auto &backend) {
        backend.command_capabilities.max_signal_points =
            rt::hal_v2_timeline_signal_capacity + 1;
      },
      rt::Status::invalid_argument, 1);
  reject_then_correct(
      [](auto &, auto &backend) {
        backend.command_capabilities.max_timelines =
            rt::hal_v2_timeline_capacity + 1;
      },
      rt::Status::invalid_argument, 1);
  reject_then_correct(
      [](auto &, auto &backend) {
        backend.command_capabilities.reserved[3] = 1;
      },
      rt::Status::invalid_argument, 1);
  reject_then_correct(
      [](auto &, auto &backend) {
        backend.command_capabilities.max_in_flight_batches = 1;
      },
      rt::Status::capacity_exceeded, 1);
  reject_then_correct(
      [](auto &, auto &backend) {
        backend.command_capabilities.completion_batch_capacity = 1;
      },
      rt::Status::capacity_exceeded, 1);
}

TEST(CommandBatch, TimelineRegistrationIsBoundedFrozenAndInstanceIsolated) {
  ConfiguredBatchRuntime one;
  one.configure();
  rt::DeviceTimelineHandle duplicate;
  EXPECT_EQ(one.runtime.register_device_timeline(
                {"command.timeline", one.backend_handle, 0}, duplicate),
            rt::Status::invalid_argument);

  ConfiguredBatchRuntime two;
  two.configure();
  EXPECT_EQ(one.runtime.register_device_timeline(
                {"foreign", two.backend_handle, 0}, duplicate),
            rt::Status::invalid_handle);
  EXPECT_EQ(one.runtime.finalize(), rt::Status::ok);
  EXPECT_EQ(one.runtime.register_device_timeline(
                {"late", one.backend_handle, 0}, duplicate),
            rt::Status::invalid_state);
  EXPECT_EQ(one.runtime.device_timeline_count(one.backend_handle), 1u);
  rt::DeviceTimelineInfo info;
  ASSERT_TRUE(one.runtime.device_timeline_at(one.backend_handle, 0, info));
  EXPECT_EQ(info.timeline, one.timeline);
  EXPECT_EQ(info.initial_value, 0u);
  EXPECT_EQ(info.last_accepted_value, 0u);
  EXPECT_EQ(info.completed_value, 0u);
}

TEST(CommandBatch, ResourceBoundsRejectSeventeenthTimelineAndNinthPhase) {
  BatchBackend backend;
  rt::Runtime runtime;
  ASSERT_EQ(runtime.configure(batch_config()), rt::Status::ok);
  rt::DeviceBackendHandle backend_handle;
  ASSERT_EQ(register_backend(runtime, backend, "bounded", backend_handle),
            rt::Status::ok);
  std::array<std::byte, 64> storage{};
  rt::DeviceBufferHandle buffer;
  ASSERT_EQ(runtime.register_device_buffer(
                {"bounded.buffer", backend_handle, storage}, buffer),
            rt::Status::ok);
  std::array<std::string, rt::hal_v2_timeline_capacity + 1> names{};
  std::array<rt::DeviceTimelineHandle, rt::hal_v2_timeline_capacity>
      timelines{};
  for (std::size_t index = 0; index < timelines.size(); ++index) {
    names[index] = "bounded.timeline." + std::to_string(index);
    ASSERT_EQ(runtime.register_device_timeline(
                  {names[index], backend_handle, 0}, timelines[index]),
              rt::Status::ok);
  }
  names.back() = "bounded.timeline.excess";
  rt::DeviceTimelineHandle excess;
  EXPECT_EQ(runtime.register_device_timeline({names.back(), backend_handle, 0},
                                             excess),
            rt::Status::capacity_exceeded);

  BatchProvider provider;
  provider.declaration = dispatch_declaration(buffer, timelines[0]);
  std::array<std::string, 9> phase_names{};
  for (std::size_t index = 0; index < 8; ++index) {
    phase_names[index] = "bounded.phase." + std::to_string(index);
    rt::PhaseHandle phase;
    ASSERT_EQ(runtime.register_device_batch_phase(
                  {phase_names[index], backend_handle, &provide_batch,
                   &provider, provider.declaration},
                  phase),
              rt::Status::ok);
  }
  phase_names.back() = "bounded.phase.excess";
  rt::PhaseHandle excess_phase;
  EXPECT_EQ(runtime.register_device_batch_phase(
                {phase_names.back(), backend_handle, &provide_batch, &provider,
                 provider.declaration},
                excess_phase),
            rt::Status::capacity_exceeded);
}

TEST(CommandBatch, DispatchCompletesOnSubmissionAndServiceLanes) {
  ConfiguredBatchRuntime fixture;
  fixture.configure();
  BatchProvider provider;
  provider.declaration = dispatch_declaration(fixture.buffer, fixture.timeline);
  rt::PhaseHandle phase;
  ASSERT_EQ(fixture.runtime.register_device_batch_phase(
                {"batch.dispatch", fixture.backend_handle, &provide_batch,
                 &provider, provider.declaration},
                phase),
            rt::Status::ok);
  ASSERT_TRUE(phase.valid());
  ASSERT_EQ(fixture.runtime.finalize(), rt::Status::ok);

  rt::MemoryPlan plan;
  ASSERT_TRUE(fixture.runtime.memory_plan(plan));
  EXPECT_EQ(plan.device_batch_backend_count, 1u);
  EXPECT_EQ(plan.device_timeline_count, 1u);
  EXPECT_EQ(plan.device_batch_queue_slots, 2u);
  rt::CpuMemoryPolicyReport report;
  ASSERT_TRUE(fixture.runtime.cpu_memory_policy_report(report));
  const auto *submission =
      find_thread(report, rt::thread_role_device_submission);
  ASSERT_NE(submission, nullptr);
  EXPECT_EQ(submission->logical_instance_count, 1u);
  EXPECT_TRUE(submission->cardinality_known);

  ASSERT_EQ(fixture.runtime.start(), rt::Status::ok);
  EXPECT_EQ(
      fixture.runtime.step({1, std::chrono::nanoseconds(1), std::nullopt}),
      rt::Status::ok);
  EXPECT_EQ(fixture.backend.batch_submit_calls.load(), 1u);
  EXPECT_EQ(fixture.backend.single_submit_calls.load(), 0u);
  EXPECT_NE(fixture.backend.last_batch.batch_id, 0u);
  EXPECT_EQ(fixture.backend.last_batch.frame_index, 1u);
  EXPECT_EQ(fixture.backend.last_batch.commands[0].buffers[0].buffer_token,
            0x1001u);
  rt::DeviceTimelineInfo info;
  ASSERT_TRUE(
      fixture.runtime.device_timeline_at(fixture.backend_handle, 0, info));
  EXPECT_EQ(info.last_accepted_value, 1u);
  EXPECT_EQ(info.completed_value, 1u);
  EXPECT_EQ(fixture.runtime.reset_device(fixture.backend_handle),
            rt::Status::ok);
  EXPECT_EQ(fixture.runtime.stop(), rt::Status::ok);
}

TEST(CommandBatch, PriorAcceptedWaitMayAdvanceTheSameTimeline) {
  ConfiguredBatchRuntime fixture;
  fixture.configure();
  BatchProvider first;
  first.declaration = dispatch_declaration(fixture.buffer, fixture.timeline);
  BatchProvider second;
  second.declaration = dispatch_declaration(fixture.buffer, fixture.timeline);
  second.declaration.wait_count = 1;
  second.declaration.waits[0].timeline_handle = fixture.timeline.value;
  second.wait_value = 1;
  second.signal_value = 2;
  rt::PhaseHandle first_phase;
  rt::PhaseHandle second_phase;
  ASSERT_EQ(fixture.runtime.register_device_batch_phase(
                {"batch.first", fixture.backend_handle, &provide_batch, &first,
                 first.declaration},
                first_phase),
            rt::Status::ok);
  ASSERT_EQ(fixture.runtime.register_device_batch_phase(
                {"batch.second", fixture.backend_handle, &provide_batch,
                 &second, second.declaration},
                second_phase),
            rt::Status::ok);
  ASSERT_EQ(fixture.runtime.add_dependency(first_phase, second_phase),
            rt::Status::ok);
  ASSERT_EQ(fixture.runtime.finalize(), rt::Status::ok);
  ASSERT_EQ(fixture.runtime.start(), rt::Status::ok);
  EXPECT_EQ(
      fixture.runtime.step({0, std::chrono::nanoseconds(1), std::nullopt}),
      rt::Status::ok);
  EXPECT_EQ(fixture.backend.batch_submit_calls.load(), 2u);
  rt::DeviceTimelineInfo info;
  ASSERT_TRUE(
      fixture.runtime.device_timeline_at(fixture.backend_handle, 0, info));
  EXPECT_EQ(info.last_accepted_value, 2u);
  EXPECT_EQ(info.completed_value, 2u);
  EXPECT_EQ(fixture.runtime.stop(), rt::Status::ok);
}

TEST(CommandBatch, SubmitErrorsAndExceptionsProduceOneTerminalResult) {
  for (const auto mode :
       {BatchBackend::SubmitMode::error, BatchBackend::SubmitMode::throwing}) {
    ConfiguredBatchRuntime fixture;
    fixture.configure();
    fixture.backend.mode.store(mode);
    BatchProvider provider;
    provider.declaration =
        dispatch_declaration(fixture.buffer, fixture.timeline);
    rt::PhaseHandle phase;
    ASSERT_EQ(fixture.runtime.register_device_batch_phase(
                  {"batch.submit-failure", fixture.backend_handle,
                   &provide_batch, &provider, provider.declaration},
                  phase),
              rt::Status::ok);
    ASSERT_EQ(fixture.runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(fixture.runtime.start(), rt::Status::ok);
    EXPECT_EQ(
        fixture.runtime.step({0, std::chrono::nanoseconds(1), std::nullopt}),
        rt::Status::device_error);
    EXPECT_EQ(fixture.backend.batch_submit_calls.load(), 1u);
    rt::DeviceTimelineInfo info;
    ASSERT_TRUE(
        fixture.runtime.device_timeline_at(fixture.backend_handle, 0, info));
    EXPECT_EQ(info.last_accepted_value, 1u);
    EXPECT_EQ(info.completed_value, 0u);
    EXPECT_EQ(fixture.runtime.stop(), rt::Status::ok);
  }
}

TEST(CommandBatch, ProviderAndCompletionFailuresAreTerminalWithoutPublication) {
  {
    ConfiguredBatchRuntime fixture;
    fixture.configure();
    BatchProvider provider;
    provider.declaration =
        dispatch_declaration(fixture.buffer, fixture.timeline);
    provider.malformed_timeout = true;
    rt::PhaseHandle phase;
    ASSERT_EQ(fixture.runtime.register_device_batch_phase(
                  {"batch.bad-provider", fixture.backend_handle, &provide_batch,
                   &provider, provider.declaration},
                  phase),
              rt::Status::ok);
    ASSERT_EQ(fixture.runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(fixture.runtime.start(), rt::Status::ok);
    EXPECT_EQ(
        fixture.runtime.step({0, std::chrono::nanoseconds(1), std::nullopt}),
        rt::Status::invalid_argument);
    EXPECT_EQ(fixture.backend.batch_submit_calls.load(), 0u);
    EXPECT_EQ(fixture.runtime.stop(), rt::Status::ok);
  }
  {
    ConfiguredBatchRuntime fixture;
    fixture.configure();
    fixture.backend.mode.store(BatchBackend::SubmitMode::malformed_completion);
    BatchProvider provider;
    provider.declaration =
        dispatch_declaration(fixture.buffer, fixture.timeline);
    rt::PhaseHandle phase;
    ASSERT_EQ(fixture.runtime.register_device_batch_phase(
                  {"batch.bad-completion", fixture.backend_handle,
                   &provide_batch, &provider, provider.declaration},
                  phase),
              rt::Status::ok);
    ASSERT_EQ(fixture.runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(fixture.runtime.start(), rt::Status::ok);
    EXPECT_EQ(
        fixture.runtime.step({0, std::chrono::nanoseconds(1), std::nullopt}),
        rt::Status::device_error);
    rt::DeviceTimelineInfo info;
    ASSERT_TRUE(
        fixture.runtime.device_timeline_at(fixture.backend_handle, 0, info));
    EXPECT_EQ(info.completed_value, 0u);
    EXPECT_EQ(fixture.runtime.stop(), rt::Status::ok);
  }
}

TEST(CommandBatch, TimeoutCancelsAndBlockedSubmitIsReleasedByStopRequest) {
  {
    ConfiguredBatchRuntime fixture;
    fixture.configure();
    fixture.backend.mode.store(BatchBackend::SubmitMode::no_completion);
    BatchProvider provider;
    provider.declaration =
        dispatch_declaration(fixture.buffer, fixture.timeline);
    provider.timeout_ns = 1'000'000;
    rt::PhaseHandle phase;
    ASSERT_EQ(fixture.runtime.register_device_batch_phase(
                  {"batch.timeout", fixture.backend_handle, &provide_batch,
                   &provider, provider.declaration},
                  phase),
              rt::Status::ok);
    ASSERT_EQ(fixture.runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(fixture.runtime.start(), rt::Status::ok);
    EXPECT_EQ(
        fixture.runtime.step({0, std::chrono::nanoseconds(1), std::nullopt}),
        rt::Status::device_timeout);
    EXPECT_EQ(fixture.backend.batch_cancel_calls.load(), 1u);
    rt::DeviceTimelineInfo info;
    ASSERT_TRUE(
        fixture.runtime.device_timeline_at(fixture.backend_handle, 0, info));
    EXPECT_EQ(info.last_accepted_value, 1u);
    EXPECT_EQ(info.completed_value, 0u);
    EXPECT_EQ(fixture.runtime.stop(), rt::Status::ok);
  }
  {
    ConfiguredBatchRuntime fixture;
    fixture.configure();
    fixture.backend.mode.store(BatchBackend::SubmitMode::blocked);
    BatchProvider provider;
    provider.declaration =
        dispatch_declaration(fixture.buffer, fixture.timeline);
    provider.timeout_ns = 1'000'000'000;
    rt::PhaseHandle phase;
    ASSERT_EQ(fixture.runtime.register_device_batch_phase(
                  {"batch.blocked", fixture.backend_handle, &provide_batch,
                   &provider, provider.declaration},
                  phase),
              rt::Status::ok);
    ASSERT_EQ(fixture.runtime.finalize(), rt::Status::ok);
    ASSERT_EQ(fixture.runtime.start(), rt::Status::ok);
    rt::Status step_status = rt::Status::ok;
    std::thread step_thread([&] {
      step_status =
          fixture.runtime.step({0, std::chrono::nanoseconds(1), std::nullopt});
    });
    while (!fixture.backend.submit_entered.load(std::memory_order_acquire)) {
      fixture.backend.submit_entered.wait(false, std::memory_order_relaxed);
    }
    EXPECT_EQ(fixture.runtime.stop(), rt::Status::invalid_state);
    step_thread.join();
    EXPECT_EQ(step_status, rt::Status::device_canceled);
    EXPECT_GE(fixture.backend.stop_request_calls.load(), 1u);
    EXPECT_EQ(fixture.runtime.stop(), rt::Status::ok);
  }
}

TEST(CommandBatch, SaturationAcceptsOnlyTheFixedPrefixWithoutRetryOrSpill) {
  ConfiguredBatchRuntime fixture;
  fixture.configure(1);
  fixture.backend.mode.store(BatchBackend::SubmitMode::no_completion);
  std::atomic<std::size_t> provider_calls{0};
  std::array<BatchProvider, 3> providers{};
  std::array<rt::DeviceTimelineHandle, 3> timelines{fixture.timeline, {}, {}};
  ASSERT_EQ(
      fixture.runtime.register_device_timeline(
          {"command.saturation.1", fixture.backend_handle, 0}, timelines[1]),
      rt::Status::ok);
  ASSERT_EQ(
      fixture.runtime.register_device_timeline(
          {"command.saturation.2", fixture.backend_handle, 0}, timelines[2]),
      rt::Status::ok);
  for (std::size_t index = 0; index < providers.size(); ++index) {
    providers[index].declaration =
        dispatch_declaration(fixture.buffer, timelines[index]);
    providers[index].signal_value = 1;
    providers[index].timeout_ns = 10'000'000;
    providers[index].callback_count = &provider_calls;
    rt::PhaseHandle phase;
    const auto name = "batch.saturation." + std::to_string(index);
    ASSERT_EQ(fixture.runtime.register_device_batch_phase(
                  {name, fixture.backend_handle, &provide_batch,
                   &providers[index], providers[index].declaration},
                  phase),
              rt::Status::ok);
  }
  ASSERT_EQ(fixture.runtime.finalize(), rt::Status::ok);
  ASSERT_EQ(fixture.runtime.start(), rt::Status::ok);
  EXPECT_EQ(
      fixture.runtime.step({0, std::chrono::nanoseconds(1), std::nullopt}),
      rt::Status::device_queue_full);
  rt::RuntimeMetricSnapshot metrics;
  rt::RuntimeMetricCursor cursor;
  ASSERT_EQ(fixture.runtime.metrics_snapshot(
                rt::RuntimeMetricWindow::cumulative, &cursor, metrics),
            rt::Status::ok);
  EXPECT_EQ(metrics.samples[static_cast<std::size_t>(
                                rt::RuntimeMetricId::device_queue_rejections)]
                .value,
            1u);
  EXPECT_EQ(provider_calls.load(), 3u);
  EXPECT_EQ(fixture.backend.batch_submit_calls.load(), 2u);
  EXPECT_EQ(fixture.backend.batch_cancel_calls.load(), 2u);
  EXPECT_EQ(fixture.runtime.stop(), rt::Status::ok);
}

TEST(CommandBatch, ExplicitFlushAndInvalidateOrderIsRequiredAndPreserved) {
  auto run = [](bool correct_order) {
    rt::Runtime runtime;
    BatchBackend backend;
    EXPECT_EQ(runtime.configure(batch_config()), rt::Status::ok);
    rt::DeviceBackendHandle backend_handle;
    EXPECT_EQ(register_backend(runtime, backend, "ordered", backend_handle),
              rt::Status::ok);
    rt::DeviceMemoryDomainHandle domain;
    rt::HalV2MemoryDomain descriptor;
    EXPECT_TRUE(
        runtime.device_memory_domain_at(backend_handle, 1, domain, descriptor));
    std::array<std::byte, 64> storage{};
    rt::DeviceBufferHandle buffer;
    EXPECT_EQ(
        runtime.register_device_buffer(
            {"ordered.buffer",
             backend_handle,
             domain,
             storage,
             {},
             storage.size(),
             rt::HalV2MemoryOwnership::borrowed_host,
             descriptor.access,
             rt::HalV2MemoryCoherency::explicit_flush_invalidate,
             rt::hal_v2_memory_sync_flush | rt::hal_v2_memory_sync_invalidate},
            buffer),
        rt::Status::ok);
    rt::DeviceTimelineHandle timeline;
    EXPECT_EQ(runtime.register_device_timeline(
                  {"ordered.timeline", backend_handle, 0}, timeline),
              rt::Status::ok);

    rt::DeviceCommandBatch declaration;
    declaration.command_count = 3;
    declaration.signal_count = 1;
    declaration.signals[0].timeline_handle = timeline.value;
    auto make_sync = [&](std::size_t index,
                         rt::HalV2MemoryOperation operation) {
      auto &command = declaration.commands[index];
      command.kind = static_cast<std::uint32_t>(
          rt::HalV2CommandKind::memory_synchronization);
      command.operation = static_cast<std::uint32_t>(operation);
      command.target =
          reference(buffer, RTFW_DEVICE_ACCESS_READ_WRITE, storage.size());
    };
    const auto flush_index = correct_order ? 0u : 1u;
    const auto dispatch_index = correct_order ? 1u : 0u;
    make_sync(flush_index, rt::HalV2MemoryOperation::flush);
    auto &dispatch = declaration.commands[dispatch_index];
    dispatch.kind = static_cast<std::uint32_t>(rt::HalV2CommandKind::dispatch);
    dispatch.opcode = 9;
    dispatch.buffer_count = 1;
    dispatch.buffers[0] =
        reference(buffer, RTFW_DEVICE_ACCESS_READ_WRITE, storage.size());
    make_sync(2, rt::HalV2MemoryOperation::invalidate);

    BatchProvider provider;
    provider.declaration = declaration;
    rt::PhaseHandle phase;
    EXPECT_EQ(runtime.register_device_batch_phase(
                  {correct_order ? "ordered.good" : "ordered.bad",
                   backend_handle, &provide_batch, &provider, declaration},
                  phase),
              rt::Status::ok);
    EXPECT_EQ(runtime.finalize(), rt::Status::ok);
    EXPECT_EQ(runtime.start(), rt::Status::ok);
    const auto status =
        runtime.step({0, std::chrono::nanoseconds(1), std::nullopt});
    EXPECT_EQ(runtime.stop(), rt::Status::ok);
    if (correct_order) {
      EXPECT_EQ(backend.batch_submit_calls.load(), 1u);
      EXPECT_EQ(backend.last_batch.command_count, 3u);
    } else {
      EXPECT_EQ(backend.batch_submit_calls.load(), 0u);
    }
    return status;
  };

  EXPECT_EQ(run(true), rt::Status::ok);
  EXPECT_EQ(run(false), rt::Status::device_error);
}
