#pragma once

#include "aligned_storage.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <rt/runtime.hpp>

namespace rt::detail {

static_assert(
    std::atomic<std::uint64_t>::is_always_lock_free,
    "rate telemetry requires lock-free 64-bit atomics");

#if defined(_MSC_VER)
// The slot and counters intentionally occupy separate cache lines. MSVC
// reports C4324 whenever an alignment specifier introduces that padding.
#    pragma warning(push)
#    pragma warning(disable : 4324)
#endif

class RateTelemetryRing final {
public:
    explicit RateTelemetryRing(std::size_t capacity);
    ~RateTelemetryRing();

    RateTelemetryRing(const RateTelemetryRing&) = delete;
    RateTelemetryRing& operator=(const RateTelemetryRing&) = delete;

    [[nodiscard]] bool emit(RateActionRecord record) noexcept;
    [[nodiscard]] bool read_sequence(
        std::uint64_t sequence,
        RateActionRecord& record) const noexcept;
    [[nodiscard]] std::uint64_t oldest_sequence(
        std::uint64_t end_sequence) const noexcept;

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] const void* slot_data() const noexcept { return slots_; }
    [[nodiscard]] std::size_t slot_storage_bytes() const noexcept {
        return capacity_ * sizeof(Slot);
    }
    [[nodiscard]] std::uint64_t next_sequence() const noexcept {
        return next_sequence_.load(std::memory_order_acquire);
    }
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
    [[nodiscard]] static constexpr std::size_t slot_alignment() noexcept;

private:
    static constexpr std::uint64_t invalid_sequence =
        std::numeric_limits<std::uint64_t>::max();
    static constexpr std::size_t word_count =
        sizeof(RateActionRecord) / sizeof(std::uint64_t);
    static_assert(sizeof(RateActionRecord) % sizeof(std::uint64_t) == 0);

    struct alignas(64) Slot {
        std::atomic_flag writer = ATOMIC_FLAG_INIT;
        std::atomic<std::uint64_t> committed_sequence{invalid_sequence};
        std::array<std::atomic<std::uint64_t>, word_count> words{};
    };

    AlignedStorage storage_{};
    Slot* slots_ = nullptr;
    std::size_t capacity_ = 0;
    alignas(64) std::atomic<std::uint64_t> next_sequence_{0};
    alignas(64) std::atomic<std::uint64_t> emitted_{0};
    alignas(64) std::atomic<std::uint64_t> overwritten_{0};
    alignas(64) std::atomic<std::uint64_t> dropped_{0};
};

#if defined(_MSC_VER)
#    pragma warning(pop)
#endif

constexpr std::size_t RateTelemetryRing::slot_size() noexcept {
    return sizeof(Slot);
}

constexpr std::size_t RateTelemetryRing::slot_alignment() noexcept {
    return alignof(Slot);
}

class RateCounters final {
public:
    RateCounters() noexcept;

    void reset(std::uint64_t policy_version) noexcept;
    [[nodiscard]] bool add(RateCounterId id, std::uint64_t amount) noexcept;
    void set(RateCounterId id, std::uint64_t value) noexcept;
    [[nodiscard]] std::uint64_t load(RateCounterId id) const noexcept;

private:
    std::array<
        std::atomic<std::uint64_t>,
        rate_action_counter_count> values_{};
};

} // namespace rt::detail
