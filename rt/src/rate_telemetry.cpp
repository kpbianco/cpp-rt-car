#include "rate_telemetry.hpp"

#include <array>
#include <bit>
#include <limits>
#include <new>
#include <stdexcept>

namespace {

using Definition = rt::RateCounterDefinition;

constexpr std::array<Definition, rt::rate_action_counter_count>
    kDefinitions{{
        {rt::RateCounterId::due_domain_releases, rt::RateCounterKind::counter,
         "rate.due_domain_releases"},
        {rt::RateCounterId::executed_reference_records, rt::RateCounterKind::counter,
         "rate.executed_reference_records"},
        {rt::RateCounterId::late_domain_releases, rt::RateCounterKind::counter,
         "rate.late_domain_releases"},
        {rt::RateCounterId::caught_up_domain_releases, rt::RateCounterKind::counter,
         "rate.caught_up_domain_releases"},
        {rt::RateCounterId::skipped_domain_releases, rt::RateCounterKind::counter,
         "rate.skipped_domain_releases"},
        {rt::RateCounterId::held_domain_releases, rt::RateCounterKind::counter,
         "rate.held_domain_releases"},
        {rt::RateCounterId::degraded_domain_releases, rt::RateCounterKind::counter,
         "rate.degraded_domain_releases"},
        {rt::RateCounterId::failed_domain_releases, rt::RateCounterKind::counter,
         "rate.failed_domain_releases"},
        {rt::RateCounterId::optional_due_domain_releases, rt::RateCounterKind::counter,
         "rate.optional_due_domain_releases"},
        {rt::RateCounterId::optional_executed_domain_releases, rt::RateCounterKind::counter,
         "rate.optional_executed_domain_releases"},
        {rt::RateCounterId::shed_domain_releases, rt::RateCounterKind::counter,
         "rate.shed_domain_releases"},
        {rt::RateCounterId::shed_transitions, rt::RateCounterKind::counter,
         "rate.shed_transitions"},
        {rt::RateCounterId::recovery_transitions, rt::RateCounterKind::counter,
         "rate.recovery_transitions"},
        {rt::RateCounterId::records_emitted, rt::RateCounterKind::counter,
         "rate.records_emitted"},
        {rt::RateCounterId::records_overwritten, rt::RateCounterKind::counter,
         "rate.records_overwritten"},
        {rt::RateCounterId::records_dropped, rt::RateCounterKind::counter,
         "rate.records_dropped"},
        {rt::RateCounterId::currently_shed_domains, rt::RateCounterKind::gauge,
         "rate.currently_shed_domains"},
        {rt::RateCounterId::policy_version, rt::RateCounterKind::gauge,
         "rate.policy_version"},
        {rt::RateCounterId::rejected_reference_records, rt::RateCounterKind::counter,
         "rate.rejected_reference_records"},
        {rt::RateCounterId::stale_reads, rt::RateCounterKind::counter,
         "rate.stale_reads"},
    }};

consteval bool definitions_are_ordered() {
    for (std::size_t index = 0; index < kDefinitions.size(); ++index) {
        if (static_cast<std::size_t>(kDefinitions[index].id) != index) {
            return false;
        }
    }
    return true;
}

static_assert(definitions_are_ordered());
static_assert(static_cast<std::uint8_t>(rt::RateActionId::optional_shed) == 6);
static_assert(static_cast<std::uint8_t>(rt::RateTransitionId::recover) == 2);
static_assert(static_cast<std::uint8_t>(rt::RateActionReason::arithmetic_failure) == 7);
static_assert(static_cast<std::uint8_t>(rt::RateCounterId::stale_reads) == 19);

} // namespace

namespace rt {

bool rate_counter_definition(
    std::size_t schema_index,
    RateCounterDefinition& definition) noexcept {
    definition = {};
    if (schema_index >= kDefinitions.size()) {
        return false;
    }
    definition = kDefinitions[schema_index];
    return true;
}

} // namespace rt

namespace rt::detail {

RateTelemetryRing::RateTelemetryRing(std::size_t capacity)
    : capacity_(capacity) {
    if (capacity > rate_telemetry_capacity_limit ||
        capacity > std::numeric_limits<std::size_t>::max() / sizeof(Slot)) {
        throw std::invalid_argument("rate telemetry capacity is invalid");
    }
    storage_.allocate(capacity * sizeof(Slot), alignof(Slot));
    slots_ = reinterpret_cast<Slot*>(storage_.data());
    for (std::size_t index = 0; index < capacity_; ++index) {
        ::new (static_cast<void*>(slots_ + index)) Slot{};
    }
}

RateTelemetryRing::~RateTelemetryRing() {
    for (std::size_t index = capacity_; index != 0; --index) {
        slots_[index - 1].~Slot();
    }
}

bool RateTelemetryRing::emit(RateActionRecord record) noexcept {
    const auto sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
    record.sequence = sequence;
    if (capacity_ == 0) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    auto& slot = slots_[static_cast<std::size_t>(
        sequence % static_cast<std::uint64_t>(capacity_))];
    if (slot.writer.test_and_set(std::memory_order_acquire)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto previous = slot.committed_sequence.exchange(
        invalid_sequence, std::memory_order_acq_rel);
    const auto words =
        std::bit_cast<std::array<std::uint64_t, word_count>>(record);
    for (std::size_t index = 0; index < words.size(); ++index) {
        slot.words[index].store(words[index], std::memory_order_relaxed);
    }
    slot.committed_sequence.store(sequence, std::memory_order_release);
    slot.writer.clear(std::memory_order_release);
    emitted_.fetch_add(1, std::memory_order_relaxed);
    if (previous != invalid_sequence) {
        overwritten_.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

bool RateTelemetryRing::read_sequence(
    std::uint64_t sequence,
    RateActionRecord& record) const noexcept {
    record = {};
    if (capacity_ == 0) {
        return false;
    }
    const auto& slot = slots_[static_cast<std::size_t>(
        sequence % static_cast<std::uint64_t>(capacity_))];
    if (slot.committed_sequence.load(std::memory_order_acquire) != sequence) {
        return false;
    }
    std::array<std::uint64_t, word_count> words{};
    for (std::size_t index = 0; index < words.size(); ++index) {
        words[index] = slot.words[index].load(std::memory_order_relaxed);
    }
    const auto candidate = std::bit_cast<RateActionRecord>(words);
    if (slot.committed_sequence.load(std::memory_order_acquire) != sequence ||
        candidate.schema_version != rate_action_schema_version ||
        candidate.record_size != sizeof(RateActionRecord) ||
        candidate.sequence != sequence) {
        return false;
    }
    record = candidate;
    return true;
}

std::uint64_t RateTelemetryRing::oldest_sequence(
    std::uint64_t end_sequence) const noexcept {
    const auto capacity = static_cast<std::uint64_t>(capacity_);
    return end_sequence > capacity ? end_sequence - capacity : 0;
}

RateCounters::RateCounters() noexcept {
    reset(0);
}

void RateCounters::reset(std::uint64_t policy_version) noexcept {
    for (auto& value : values_) {
        value.store(0, std::memory_order_relaxed);
    }
    set(RateCounterId::policy_version, policy_version);
}

bool RateCounters::add(RateCounterId id, std::uint64_t amount) noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index >= values_.size() || index == 16 || index == 17) {
        return false;
    }
    auto current = values_[index].load(std::memory_order_relaxed);
    for (;;) {
        if (amount > std::numeric_limits<std::uint64_t>::max() - current) {
            return false;
        }
        if (values_[index].compare_exchange_weak(
                current, current + amount, std::memory_order_relaxed)) {
            return true;
        }
    }
}

void RateCounters::set(RateCounterId id, std::uint64_t value) noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index < values_.size()) {
        values_[index].store(value, std::memory_order_release);
    }
}

std::uint64_t RateCounters::load(RateCounterId id) const noexcept {
    const auto index = static_cast<std::size_t>(id);
    return index < values_.size()
        ? values_[index].load(std::memory_order_acquire)
        : 0;
}

} // namespace rt::detail
