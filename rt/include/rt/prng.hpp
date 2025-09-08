#pragma once
#include <array>
#include <cstdint>

namespace rt::prng {

namespace detail {
inline void round(std::array<std::uint32_t,4> &c, std::array<std::uint32_t,2> &k) {
    const std::uint64_t M0 = 0xD2511F53u;
    const std::uint64_t M1 = 0xCD9E8D57u;
    std::uint64_t p0 = M0 * static_cast<std::uint64_t>(c[0]);
    std::uint64_t p1 = M1 * static_cast<std::uint64_t>(c[2]);
    std::uint32_t hi0 = static_cast<std::uint32_t>(p0 >> 32);
    std::uint32_t lo0 = static_cast<std::uint32_t>(p0);
    std::uint32_t hi1 = static_cast<std::uint32_t>(p1 >> 32);
    std::uint32_t lo1 = static_cast<std::uint32_t>(p1);
    c[0] = hi1 ^ c[1] ^ k[0];
    c[1] = lo1;
    c[2] = hi0 ^ c[3] ^ k[1];
    c[3] = lo0;
    k[0] += 0x9E3779B9u;
    k[1] += 0xBB67AE85u;
}
} // namespace detail

// Philox4x32 with 10 rounds. Counter and key are derived from 64-bit
// values for convenience.
inline std::array<std::uint32_t,4> philox4x32_10(std::uint64_t key64,
                                                std::uint64_t ctr64) {
    std::array<std::uint32_t,4> ctr{static_cast<std::uint32_t>(ctr64),
                                    static_cast<std::uint32_t>(ctr64 >> 32),
                                    0u, 0u};
    std::array<std::uint32_t,2> key{static_cast<std::uint32_t>(key64),
                                    static_cast<std::uint32_t>(key64 >> 32)};
    for (int i = 0; i < 10; ++i)
        detail::round(ctr, key);
    return ctr;
}

inline std::uint32_t uniform_u32(std::uint64_t seed, std::uint64_t counter) {
    auto out = philox4x32_10(seed, counter);
    return out[0];
}

inline double uniform_double(std::uint64_t seed, std::uint64_t counter) {
    std::uint64_t r = static_cast<std::uint64_t>(uniform_u32(seed, counter)) << 32;
    r |= static_cast<std::uint64_t>(uniform_u32(seed, counter + 1));
    const double scale = 1.0 / static_cast<double>(std::uint64_t(-1));
    return static_cast<double>(r) * scale;
}

} // namespace rt::prng

