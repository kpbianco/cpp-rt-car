#pragma once
#include <vector>
#include <cstddef>
#include <type_traits>

namespace detreduce {

// Classic pairwise (binary-tree) summation in a fixed order.
// Deterministic given the same input array order.
template <class T>
T pairwise_sum(const T* data, std::size_t n)
{
    static_assert(std::is_floating_point_v<T> || std::is_integral_v<T>,
                  "pairwise_sum expects arithmetic types");
    if (n == 0) return T(0);

    // Iterative binary-tree reduction to avoid deep recursion.
    // Copy into a working buffer to do in-place level-by-level combines.
    std::vector<T> level(data, data + n);

    while (level.size() > 1) {
        std::size_t m = level.size();
        std::size_t pairs = m / 2;
        std::size_t remain = m % 2;
        std::size_t outN = pairs + remain;
        std::vector<T> next;
        next.resize(outN);

        // Combine adjacent pairs (0+1, 2+3, 4+5, ...)
        for (std::size_t i = 0; i < pairs; ++i) {
            std::size_t a = 2 * i;
            std::size_t b = a + 1;
            next[i] = level[a] + level[b];
        }
        // Carry the odd last element if any
        if (remain) next[pairs] = level[m - 1];

        level.swap(next);
    }
    return level[0];
}

// Optional: Neumaier compensated sum (still deterministic) for better accuracy.
template <class T>
T neumaier_sum(const T* data, std::size_t n)
{
    static_assert(std::is_floating_point_v<T>, "neumaier_sum expects float/double");
    T sum = 0;
    T c = 0; // compensation
    for (std::size_t i = 0; i < n; ++i) {
        T t = sum + data[i];
        // Larger magnitude first
        if (std::abs(sum) >= std::abs(data[i])) c += (sum - t) + data[i];
        else                                     c += (data[i] - t) + sum;
        sum = t;
    }
    return sum + c;
}

} // namespace detreduce
