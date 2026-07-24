#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

#include <rt/runtime.hpp>

namespace rt::detail {

static_assert(
    std::atomic<std::uint16_t>::is_always_lock_free &&
        std::atomic<std::uint32_t>::is_always_lock_free &&
        std::atomic<std::int32_t>::is_always_lock_free &&
        std::atomic<std::uint64_t>::is_always_lock_free,
    "versioned observability requires lock-free fixed-width atomics");

class TelemetryRing final {
public:
    explicit TelemetryRing(std::size_t capacity);

    TelemetryRing(const TelemetryRing&) = delete;
    TelemetryRing& operator=(const TelemetryRing&) = delete;

    [[nodiscard]] bool emit(RuntimeTraceEvent event) noexcept;
    [[nodiscard]] bool read_sequence(
        std::uint64_t sequence,
        RuntimeTraceEvent& event) const noexcept;
    [[nodiscard]] bool event_at(
        std::size_t chronological_index,
        RuntimeTraceEvent& event) const noexcept;

    [[nodiscard]] std::size_t retained_count() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }
    [[nodiscard]] std::uint64_t next_sequence() const noexcept {
        return next_sequence_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t oldest_sequence(
        std::uint64_t end_sequence) const noexcept;
    [[nodiscard]] std::uint64_t emitted() const noexcept {
        return emitted_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t overwritten() const noexcept {
        return overwritten_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_acquire);
    }

    [[nodiscard]] static constexpr std::size_t slot_size() noexcept;

private:
    static constexpr std::uint64_t invalid_sequence =
        std::numeric_limits<std::uint64_t>::max();

    struct alignas(64) Slot {
        std::atomic_flag writer = ATOMIC_FLAG_INIT;
        std::atomic<std::uint64_t> committed_sequence{invalid_sequence};
        std::atomic<std::uint32_t> schema_version{
            observability_schema_version};
        std::atomic<std::uint16_t> record_size{64};
        std::atomic<std::uint16_t> type{
            static_cast<std::uint16_t>(
                RuntimeTraceEventType::step_begin)};
        std::atomic<std::int32_t> status{
            static_cast<std::int32_t>(Status::ok)};
        std::atomic<std::uint16_t> producer{
            static_cast<std::uint16_t>(
                RuntimeTraceProducer::host)};
        std::atomic<std::uint64_t> timestamp_ns{0};
        std::atomic<std::uint64_t> frame_index{0};
        std::atomic<std::uint32_t> callback_index{
            std::numeric_limits<std::uint32_t>::max()};
        std::atomic<std::uint32_t> worker_index{
            std::numeric_limits<std::uint32_t>::max()};
        std::atomic<std::uint64_t> value{0};
    };

    std::unique_ptr<Slot[]> slots_;
    std::size_t capacity_ = 0;
    alignas(64) std::atomic<std::uint64_t> next_sequence_{0};
    alignas(64) std::atomic<std::uint64_t> emitted_{0};
    alignas(64) std::atomic<std::uint64_t> overwritten_{0};
    alignas(64) std::atomic<std::uint64_t> dropped_{0};
};

constexpr std::size_t TelemetryRing::slot_size() noexcept {
    return sizeof(Slot);
}

class TelemetryCounters final {
public:
    TelemetryCounters() noexcept;

    void reset() noexcept;
    void increment(
        RuntimeMetricId id,
        std::uint64_t amount = 1) noexcept;
    [[nodiscard]] std::uint64_t load(
        RuntimeMetricId id) const noexcept;

private:
    std::array<
        std::atomic<std::uint64_t>,
        runtime_metric_count> values_{};
};

} // namespace rt::detail
