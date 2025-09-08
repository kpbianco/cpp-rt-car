#pragma once

// Lightweight strong type units for time and distance.
// In a production system, consider using the mp-units library instead.

#include <cstdint>
#include <type_traits>

namespace core {

// Generic quantity wrapper enforcing dimensional correctness.
template <typename Unit, typename Rep = double>
struct quantity {
    Rep value;
    constexpr explicit quantity(Rep v = {}) : value(v) {}
    constexpr Rep count() const { return value; }
};

// Arithmetic operators - only same units are permitted

template <typename Unit, typename Rep>
constexpr quantity<Unit, Rep> operator+(quantity<Unit, Rep> lhs, quantity<Unit, Rep> rhs) {
    return quantity<Unit, Rep>{lhs.count() + rhs.count()};
}

template <typename Unit, typename Rep>
constexpr quantity<Unit, Rep> operator-(quantity<Unit, Rep> lhs, quantity<Unit, Rep> rhs) {
    return quantity<Unit, Rep>{lhs.count() - rhs.count()};
}

template <typename Unit, typename Rep, typename Scalar,
          typename = std::enable_if_t<std::is_arithmetic_v<Scalar>>>
constexpr quantity<Unit, Rep> operator*(quantity<Unit, Rep> q, Scalar s) {
    return quantity<Unit, Rep>{q.count() * static_cast<Rep>(s)};
}

template <typename Unit, typename Rep, typename Scalar,
          typename = std::enable_if_t<std::is_arithmetic_v<Scalar>>>
constexpr quantity<Unit, Rep> operator*(Scalar s, quantity<Unit, Rep> q) {
    return q * s;
}

template <typename Unit, typename Rep, typename Scalar,
          typename = std::enable_if_t<std::is_arithmetic_v<Scalar>>>
constexpr quantity<Unit, Rep> operator/(quantity<Unit, Rep> q, Scalar s) {
    return quantity<Unit, Rep>{q.count() / static_cast<Rep>(s)};
}

template <typename Unit, typename Rep>
constexpr bool operator==(quantity<Unit, Rep> lhs, quantity<Unit, Rep> rhs) {
    return lhs.count() == rhs.count();
}

// Unit tags
struct seconds_tag {};
struct meters_tag {};

// Convenient aliases
using seconds = quantity<seconds_tag, double>;
using meters  = quantity<meters_tag, double>;

} // namespace core

