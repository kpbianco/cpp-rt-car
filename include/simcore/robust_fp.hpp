#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace robust {

// Return 1/sqrt(x) but clamp the denominator to avoid Inf/NaN for tiny values.
inline double safe_rsqrt(double x, double eps = 1e-12) {
    if (x <= eps) x = eps;
    return 1.0 / std::sqrt(x);
}

// Accumulate a range of float values into double precision. This makes
// the mixed-precision policy explicit for code paths where FP32 data is
// reduced using FP64 accumulation.
template <class It>
double sum_fp32_to_fp64(It begin, It end) {
    double acc = 0.0;
    for (It it = begin; it != end; ++it) {
        acc += static_cast<double>(*it);
    }
    return acc;
}

// Kahan/Babushka summation for a range of float values with a double
// accumulator. Useful for highly sensitive reductions where classical
// FP32->FP64 promotion is still not accurate enough.
template <class It>
double kahan_sum_fp32(It begin, It end) {
    double sum = 0.0;
    double c = 0.0; // compensation
    for (It it = begin; it != end; ++it) {
        double val = static_cast<double>(*it);
        double y = val - c;
        double t = sum + y;
        c = (t - sum) - y;
        sum = t;
    }
    return sum;
}

// Classic Kahan summation for improved accuracy.
template <class It>
double kahan_sum(It begin, It end) {
    double sum = 0.0;
    double c = 0.0; // compensation
    for (It it = begin; it != end; ++it) {
        double y = *it - c;
        double t = sum + y;
        c = (t - sum) - y;
        sum = t;
    }
    return sum;
}

// Compute the distance in ULPs between two doubles.
inline std::uint64_t ulp_distance(double a, double b) {
    std::uint64_t ia, ib;
    std::memcpy(&ia, &a, sizeof(a));
    std::memcpy(&ib, &b, sizeof(b));
    // Make lexicographically ordered as two's complement
    if (static_cast<std::int64_t>(ia) < 0) ia = 0x8000000000000000ULL - ia;
    if (static_cast<std::int64_t>(ib) < 0) ib = 0x8000000000000000ULL - ib;
    return (ia > ib) ? ia - ib : ib - ia;
}

} // namespace robust

