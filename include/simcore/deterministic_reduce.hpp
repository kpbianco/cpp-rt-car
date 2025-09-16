#pragma once

#include <cstddef>
#include <type_traits>
#include <vector>

namespace simcore::deterministic_reduce {

namespace detail {

template <typename T>
struct is_supported
    : std::bool_constant<std::is_same_v<std::remove_cv_t<T>, float> ||
                         std::is_same_v<std::remove_cv_t<T>, double>> {};

} // namespace detail

// Pairwise (binary-tree) reduction performed in-place.
// The order of operations is fixed, yielding deterministic results regardless of
// the execution schedule as long as the leaf inputs are identical.
template <typename T>
inline T pairwise_sum_inplace(T *values, std::size_t count) {
  static_assert(detail::is_supported<T>::value,
                "pairwise_sum_inplace expects float or double");
  if (count == 0)
    return T(0);
  std::size_t n = count;
  while (n > 1) {
    const std::size_t pairs = n / 2;
    for (std::size_t i = 0; i < pairs; ++i) {
      const std::size_t idx = 2 * i;
      const T left = values[idx];
      const T right = values[idx + 1];
      values[i] = left + right;
    }
    if (n & 1)
      values[pairs] = values[n - 1];
    n = pairs + (n & 1);
  }
  return values[0];
}

template <typename T>
inline T pairwise_sum(const T *values, std::size_t count) {
  static_assert(detail::is_supported<T>::value,
                "pairwise_sum expects float or double");
  if (count == 0)
    return T(0);
  std::vector<T> buffer(values, values + count);
  return pairwise_sum_inplace(buffer.data(), buffer.size());
}

template <typename Container>
inline auto pairwise_sum(Container &container)
    -> std::enable_if_t<detail::is_supported<typename Container::value_type>::value,
                        typename Container::value_type> {
  return pairwise_sum_inplace(container.data(), container.size());
}

template <typename Container>
inline auto pairwise_sum(const Container &container)
    -> std::enable_if_t<detail::is_supported<typename Container::value_type>::value,
                        typename Container::value_type> {
  return pairwise_sum(container.data(), container.size());
}

} // namespace simcore::deterministic_reduce
