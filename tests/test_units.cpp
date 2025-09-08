#include <gtest/gtest.h>
#include <core/units.hpp>
#include <type_traits>
#include <utility>

using namespace core;

namespace {
// Detection idiom to test if addition is valid at compile time
template <typename T, typename U, typename = void>
struct can_add : std::false_type {};

template <typename T, typename U>
struct can_add<T, U, std::void_t<decltype(std::declval<T>() + std::declval<U>())>> : std::true_type {};
} // namespace

static_assert(can_add<meters, meters>::value, "meters + meters should compile");
static_assert(!can_add<meters, seconds>::value, "meters + seconds should be ill-formed");

TEST(Units, Addition) {
    meters a{1.0};
    meters b{2.0};
    auto c = a + b;
    EXPECT_DOUBLE_EQ(c.count(), 3.0);
}

