#include "telemetry.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

namespace {

using Definition = rt::RuntimeMetricDefinition;

constexpr std::array<Definition, rt::runtime_metric_count>
    kMetricDefinitions{{
        {rt::RuntimeMetricId::frames_started,
         rt::RuntimeMetricKind::counter,
         "runtime.frames_started"},
        {rt::RuntimeMetricId::frames_completed,
         rt::RuntimeMetricKind::counter,
         "runtime.frames_completed"},
        {rt::RuntimeMetricId::frames_failed,
         rt::RuntimeMetricKind::counter,
         "runtime.frames_failed"},
        {rt::RuntimeMetricId::callbacks_started,
         rt::RuntimeMetricKind::counter,
         "runtime.callbacks_started"},
        {rt::RuntimeMetricId::callbacks_completed,
         rt::RuntimeMetricKind::counter,
         "runtime.callbacks_completed"},
        {rt::RuntimeMetricId::callback_failures,
         rt::RuntimeMetricKind::counter,
         "runtime.callback_failures"},
        {rt::RuntimeMetricId::deadline_misses,
         rt::RuntimeMetricKind::counter,
         "runtime.deadline_misses"},
        {rt::RuntimeMetricId::watchdog_events,
         rt::RuntimeMetricKind::counter,
         "runtime.watchdog_events"},
        {rt::RuntimeMetricId::degradation_events,
         rt::RuntimeMetricKind::counter,
         "runtime.degradation_events"},
        {rt::RuntimeMetricId::periodic_releases,
         rt::RuntimeMetricKind::counter,
         "runtime.periodic_releases"},
        {rt::RuntimeMetricId::periodic_wakes,
         rt::RuntimeMetricKind::counter,
         "runtime.periodic_wakes"},
        {rt::RuntimeMetricId::trace_events_emitted,
         rt::RuntimeMetricKind::counter,
         "trace.events_emitted"},
        {rt::RuntimeMetricId::trace_events_overwritten,
         rt::RuntimeMetricKind::counter,
         "trace.events_overwritten"},
        {rt::RuntimeMetricId::trace_events_dropped,
         rt::RuntimeMetricKind::counter,
         "trace.events_dropped"},
        {rt::RuntimeMetricId::executor_submitted_tasks,
         rt::RuntimeMetricKind::counter,
         "executor.submitted_tasks"},
        {rt::RuntimeMetricId::executor_local_executions,
         rt::RuntimeMetricKind::counter,
         "executor.local_executions"},
        {rt::RuntimeMetricId::executor_steal_attempts,
         rt::RuntimeMetricKind::counter,
         "executor.steal_attempts"},
        {rt::RuntimeMetricId::executor_successful_steals,
         rt::RuntimeMetricKind::counter,
         "executor.successful_steals"},
        {rt::RuntimeMetricId::executor_queue_rejections,
         rt::RuntimeMetricKind::counter,
         "executor.queue_rejections"},
        {rt::RuntimeMetricId::executor_scratch_exhaustions,
         rt::RuntimeMetricKind::counter,
         "executor.scratch_exhaustions"},
        {rt::RuntimeMetricId::executor_worker_starts,
         rt::RuntimeMetricKind::counter,
         "executor.worker_starts"},
        {rt::RuntimeMetricId::degradation_level,
         rt::RuntimeMetricKind::gauge,
         "runtime.degradation_level"},
    }};

static_assert(
    static_cast<std::size_t>(
        rt::RuntimeMetricId::count) ==
    kMetricDefinitions.size());

consteval bool metric_definitions_are_ordered() {
    for (std::size_t index = 0;
         index < kMetricDefinitions.size();
         ++index) {
        if (static_cast<std::size_t>(
                kMetricDefinitions[index].id) != index) {
            return false;
        }
    }
    return true;
}

static_assert(metric_definitions_are_ordered());

} // namespace

namespace rt {

bool runtime_metric_definition(
    std::size_t schema_index,
    RuntimeMetricDefinition& definition) noexcept {
    definition = {};
    if (schema_index >= kMetricDefinitions.size()) {
        return false;
    }
    definition = kMetricDefinitions[schema_index];
    return true;
}

const char* runtime_trace_event_name(
    RuntimeTraceEventType type) noexcept {
    switch (type) {
    case RuntimeTraceEventType::finalized:
        return "runtime.finalized";
    case RuntimeTraceEventType::started:
        return "runtime.started";
    case RuntimeTraceEventType::periodic_release:
        return "periodic.release";
    case RuntimeTraceEventType::periodic_wake:
        return "periodic.wake";
    case RuntimeTraceEventType::step_begin:
        return "frame.begin";
    case RuntimeTraceEventType::callback_begin:
        return "callback.begin";
    case RuntimeTraceEventType::callback_end:
        return "callback.end";
    case RuntimeTraceEventType::watchdog_fired:
        return "watchdog.fired";
    case RuntimeTraceEventType::degradation_applied:
        return "degradation.applied";
    case RuntimeTraceEventType::step_end:
        return "frame.end";
    case RuntimeTraceEventType::stopped:
        return "runtime.stopped";
    }
    return "unknown";
}

} // namespace rt

namespace rt::detail {

TelemetryRing::TelemetryRing(std::size_t capacity)
    : slots_(
          capacity == 0
              ? nullptr
              : std::make_unique<Slot[]>(capacity)),
      capacity_(capacity) {}

bool TelemetryRing::emit(RuntimeTraceEvent event) noexcept {
    const auto sequence =
        next_sequence_.fetch_add(1, std::memory_order_relaxed);
    event.sequence = sequence;
    if (capacity_ == 0) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    Slot& slot = slots_[static_cast<std::size_t>(
        sequence % static_cast<std::uint64_t>(capacity_))];
    if (slot.writer.test_and_set(std::memory_order_acquire)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const auto previous =
        slot.committed_sequence.exchange(
            invalid_sequence,
            std::memory_order_acq_rel);
    slot.schema_version.store(
        event.schema_version,
        std::memory_order_relaxed);
    slot.record_size.store(
        event.record_size,
        std::memory_order_relaxed);
    slot.type.store(
        static_cast<std::uint16_t>(event.type),
        std::memory_order_relaxed);
    slot.status.store(
        static_cast<std::int32_t>(event.status),
        std::memory_order_relaxed);
    slot.producer.store(
        static_cast<std::uint16_t>(event.producer),
        std::memory_order_relaxed);
    slot.timestamp_ns.store(
        event.timestamp_ns,
        std::memory_order_relaxed);
    slot.frame_index.store(
        event.frame_index,
        std::memory_order_relaxed);
    slot.callback_index.store(
        event.callback_index,
        std::memory_order_relaxed);
    slot.worker_index.store(
        event.worker_index,
        std::memory_order_relaxed);
    slot.value.store(event.value, std::memory_order_relaxed);
    slot.committed_sequence.store(
        sequence,
        std::memory_order_release);
    slot.writer.clear(std::memory_order_release);

    emitted_.fetch_add(1, std::memory_order_relaxed);
    if (previous != invalid_sequence) {
        overwritten_.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

bool TelemetryRing::read_sequence(
    std::uint64_t sequence,
    RuntimeTraceEvent& event) const noexcept {
    event = {};
    if (capacity_ == 0) {
        return false;
    }
    const Slot& slot = slots_[static_cast<std::size_t>(
        sequence % static_cast<std::uint64_t>(capacity_))];
    if (slot.committed_sequence.load(std::memory_order_acquire) !=
        sequence) {
        return false;
    }

    RuntimeTraceEvent candidate;
    candidate.schema_version =
        slot.schema_version.load(std::memory_order_relaxed);
    candidate.record_size =
        slot.record_size.load(std::memory_order_relaxed);
    candidate.type = static_cast<RuntimeTraceEventType>(
        slot.type.load(std::memory_order_relaxed));
    candidate.status = static_cast<Status>(
        slot.status.load(std::memory_order_relaxed));
    candidate.producer = static_cast<RuntimeTraceProducer>(
        slot.producer.load(std::memory_order_relaxed));
    candidate.sequence = sequence;
    candidate.timestamp_ns =
        slot.timestamp_ns.load(std::memory_order_relaxed);
    candidate.frame_index =
        slot.frame_index.load(std::memory_order_relaxed);
    candidate.callback_index =
        slot.callback_index.load(std::memory_order_relaxed);
    candidate.worker_index =
        slot.worker_index.load(std::memory_order_relaxed);
    candidate.value =
        slot.value.load(std::memory_order_relaxed);

    if (slot.committed_sequence.load(std::memory_order_acquire) !=
        sequence) {
        return false;
    }
    event = candidate;
    return true;
}

std::uint64_t TelemetryRing::oldest_sequence(
    std::uint64_t end_sequence) const noexcept {
    const auto capacity =
        static_cast<std::uint64_t>(capacity_);
    return end_sequence > capacity
        ? end_sequence - capacity
        : 0;
}

std::size_t TelemetryRing::retained_count() const noexcept {
    const auto end = next_sequence();
    const auto oldest = oldest_sequence(end);
    std::size_t count = 0;
    RuntimeTraceEvent ignored;
    for (auto sequence = oldest;
         sequence < end;
         ++sequence) {
        count += read_sequence(sequence, ignored) ? 1u : 0u;
    }
    return count;
}

bool TelemetryRing::event_at(
    std::size_t chronological_index,
    RuntimeTraceEvent& event) const noexcept {
    const auto end = next_sequence();
    const auto oldest = oldest_sequence(end);
    std::size_t found = 0;
    RuntimeTraceEvent candidate;
    for (auto sequence = oldest;
         sequence < end;
         ++sequence) {
        if (!read_sequence(sequence, candidate)) {
            continue;
        }
        if (found == chronological_index) {
            event = candidate;
            return true;
        }
        ++found;
    }
    return false;
}

TelemetryCounters::TelemetryCounters() noexcept {
    reset();
}

void TelemetryCounters::reset() noexcept {
    for (auto& value : values_) {
        value.store(0, std::memory_order_relaxed);
    }
}

void TelemetryCounters::increment(
    RuntimeMetricId id,
    std::uint64_t amount) noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index < values_.size()) {
        values_[index].fetch_add(
            amount,
            std::memory_order_relaxed);
    }
}

std::uint64_t TelemetryCounters::load(
    RuntimeMetricId id) const noexcept {
    const auto index = static_cast<std::size_t>(id);
    return index < values_.size()
        ? values_[index].load(std::memory_order_acquire)
        : 0;
}

} // namespace rt::detail
