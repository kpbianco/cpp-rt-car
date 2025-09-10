#pragma once
#include <cstdint>
#include <cmath>

namespace rt {

// Simple fixed-point type with configurable fractional bits. This is
// intended for hard real-time paths where floating point determinism is
// not sufficient.

template <int FractionBits>
class FixedPoint {
    using storage_t = std::int32_t;
    storage_t value_;
    static constexpr storage_t scale = storage_t{1} << FractionBits;

public:
    constexpr FixedPoint() : value_(0) {}
    static constexpr int frac_bits = FractionBits;

    static constexpr FixedPoint fromRaw(storage_t v) {
        FixedPoint fp;
        fp.value_ = v;
        return fp;
    }

    static constexpr FixedPoint fromFloat(float f) {
        return fromRaw(static_cast<storage_t>(std::llround(f * scale)));
    }

    float toFloat() const {
        return static_cast<float>(value_) / static_cast<float>(scale);
    }

    constexpr FixedPoint operator+(FixedPoint rhs) const {
        return fromRaw(value_ + rhs.value_);
    }
    constexpr FixedPoint operator-(FixedPoint rhs) const {
        return fromRaw(value_ - rhs.value_);
    }
    constexpr FixedPoint operator*(FixedPoint rhs) const {
        std::int64_t prod = static_cast<std::int64_t>(value_) * rhs.value_;
        return fromRaw(static_cast<storage_t>(prod >> FractionBits));
    }

    constexpr bool operator==(FixedPoint rhs) const {
        return value_ == rhs.value_;
    }

    storage_t raw() const { return value_; }
};

using Q16_16 = FixedPoint<16>;

} // namespace rt

