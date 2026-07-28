#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rt/canonical_bytes.hpp>

namespace rt {

// Legacy native-layout helpers retained for SimCore compatibility. Readers
// are bounds checked and never resize from an encoded count until the complete
// payload is known to fit the supplied input. New integrations should use the
// versioned Runtime checkpoint/replay API instead.
struct SnapshotWriter {
    std::vector<std::uint8_t> data;

    template <class T> void write(const T &v) {
        static_assert(std::is_trivially_copyable_v<T>, "write requires POD");
        const auto *p = reinterpret_cast<const std::uint8_t *>(&v);
        data.insert(data.end(), p, p + sizeof(T));
    }

    template <class T> void writeVector(const std::vector<T> &vec) {
        std::uint64_t n = vec.size();
        write(n);
        if (n) {
            static_assert(std::is_trivially_copyable_v<T>, "writeVector requires POD");
            const auto *p = reinterpret_cast<const std::uint8_t *>(vec.data());
            data.insert(data.end(), p, p + n * sizeof(T));
        }
    }

    template <class K, class V>
    void writeMap(const std::unordered_map<K, V> &m) {
        std::vector<std::pair<K, V>> items(m.begin(), m.end());
        std::sort(items.begin(), items.end(), [](auto &a, auto &b) {
            return a.first < b.first;
        });
        std::uint64_t n = items.size();
        write(n);
        for (auto &kv : items) {
            write(kv.first);
            write(kv.second);
        }
    }
};

struct SnapshotReader {
    const std::vector<std::uint8_t> &data;
    std::size_t offset{0};
    bool valid{true};

    explicit SnapshotReader(const std::vector<std::uint8_t> &d) : data(d) {}

    template <class T> void read(T &out) {
        static_assert(std::is_trivially_copyable_v<T>, "read requires POD");
        if (!valid || sizeof(T) > data.size() - std::min(offset, data.size())) {
            out = T{};
            offset = data.size();
            valid = false;
            return;
        }
        std::memcpy(&out, data.data() + offset, sizeof(T));
        offset += sizeof(T);
    }

    template <class T> void readVector(std::vector<T> &vec) {
        static_assert(std::is_trivially_copyable_v<T>, "readVector requires POD");
        std::uint64_t n = 0;
        read(n);
        if (!valid ||
            n > std::numeric_limits<std::size_t>::max() ||
            static_cast<std::size_t>(n) >
                (data.size() - offset) / sizeof(T)) {
            vec.clear();
            offset = data.size();
            valid = false;
            return;
        }
        const auto count = static_cast<std::size_t>(n);
        vec.resize(count);
        if (count != 0) {
            const auto bytes = count * sizeof(T);
            std::memcpy(vec.data(), data.data() + offset, bytes);
            offset += bytes;
        }
    }

    template <class K, class V> void readMap(std::unordered_map<K, V> &m) {
        static_assert(std::is_trivially_copyable_v<K>, "readMap requires POD keys");
        static_assert(std::is_trivially_copyable_v<V>, "readMap requires POD values");
        std::uint64_t n = 0;
        read(n);
        m.clear();
        constexpr std::size_t pair_bytes = sizeof(K) + sizeof(V);
        if (!valid ||
            n > std::numeric_limits<std::size_t>::max() ||
            static_cast<std::size_t>(n) >
                (data.size() - offset) / pair_bytes) {
            offset = data.size();
            valid = false;
            return;
        }
        m.reserve(static_cast<std::size_t>(n));
        for (std::uint64_t i = 0; i < n; ++i) {
            K k{};
            V v{};
            read(k);
            read(v);
            if (!valid) {
                m.clear();
                return;
            }
            m.emplace(std::move(k), std::move(v));
        }
    }

    [[nodiscard]] bool good() const noexcept { return valid; }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return offset <= data.size() ? data.size() - offset : 0;
    }
};

inline std::uint64_t hash64(std::span<const std::byte> buf) noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (std::byte b : buf) {
        h ^= static_cast<std::uint8_t>(b);
        h *= 1099511628211ull;
    }
    return h;
}

inline std::uint64_t hash64(const std::vector<std::uint8_t> &buf) noexcept {
    return hash64(std::as_bytes(std::span(buf)));
}

} // namespace rt
