#pragma once

#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace rt::detail {

[[nodiscard]] inline bool checked_add(
    std::size_t left,
    std::size_t right,
    std::size_t& result) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        result = 0;
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] inline bool checked_multiply(
    std::size_t left,
    std::size_t right,
    std::size_t& result) noexcept {
    if (left != 0 &&
        right > std::numeric_limits<std::size_t>::max() / left) {
        result = 0;
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] inline bool checked_align_up(
    std::size_t value,
    std::size_t alignment,
    std::size_t& result) noexcept {
    if (alignment == 0 ||
        (alignment & (alignment - 1)) != 0) {
        result = 0;
        return false;
    }
    const auto mask = alignment - 1;
    std::size_t with_padding = 0;
    if (!checked_add(value, mask, with_padding)) {
        result = 0;
        return false;
    }
    result = with_padding & ~mask;
    return true;
}

// Owns one allocation from aligned operator new and always releases it through
// the matching aligned operator delete. Allocation and page touching happen
// only while the runtime is being finalized.
class AlignedStorage final {
public:
    AlignedStorage() = default;

    AlignedStorage(std::size_t bytes, std::size_t alignment) {
        allocate(bytes, alignment);
    }

    ~AlignedStorage() {
        reset();
    }

    AlignedStorage(const AlignedStorage&) = delete;
    AlignedStorage& operator=(const AlignedStorage&) = delete;

    AlignedStorage(AlignedStorage&& other) noexcept {
        move_from(other);
    }

    AlignedStorage& operator=(AlignedStorage&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(other);
        }
        return *this;
    }

    void allocate(std::size_t bytes, std::size_t alignment) {
        reset();
        if (bytes == 0) {
            alignment_ = alignment;
            return;
        }

        auto* allocation = static_cast<std::byte*>(
            ::operator new(bytes, std::align_val_t(alignment)));
        std::memset(allocation, 0, bytes);
        data_ = allocation;
        size_ = bytes;
        alignment_ = alignment;
    }

    void reset() noexcept {
        if (data_) {
            ::operator delete(
                data_,
                std::align_val_t(alignment_));
        }
        data_ = nullptr;
        size_ = 0;
        alignment_ = alignof(std::max_align_t);
    }

    [[nodiscard]] std::byte* data() noexcept {
        return data_;
    }

    [[nodiscard]] const std::byte* data() const noexcept {
        return data_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] std::size_t alignment() const noexcept {
        return alignment_;
    }

private:
    void move_from(AlignedStorage& other) noexcept {
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
        alignment_ = std::exchange(
            other.alignment_,
            alignof(std::max_align_t));
    }

    std::byte* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t alignment_ = alignof(std::max_align_t);
};

} // namespace rt::detail
