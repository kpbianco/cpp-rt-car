#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rt {

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

    explicit SnapshotReader(const std::vector<std::uint8_t> &d) : data(d) {}

    template <class T> void read(T &out) {
        static_assert(std::is_trivially_copyable_v<T>, "read requires POD");
        std::memcpy(&out, data.data() + offset, sizeof(T));
        offset += sizeof(T);
    }

    template <class T> void readVector(std::vector<T> &vec) {
        std::uint64_t n = 0;
        read(n);
        vec.resize(static_cast<std::size_t>(n));
        if (n) {
            static_assert(std::is_trivially_copyable_v<T>, "readVector requires POD");
            std::memcpy(vec.data(), data.data() + offset, n * sizeof(T));
            offset += n * sizeof(T);
        }
    }

    template <class K, class V> void readMap(std::unordered_map<K, V> &m) {
        std::uint64_t n = 0;
        read(n);
        m.clear();
        m.reserve(static_cast<std::size_t>(n));
        for (std::uint64_t i = 0; i < n; ++i) {
            K k;
            V v;
            read(k);
            read(v);
            m.emplace(std::move(k), std::move(v));
        }
    }
};

inline std::uint64_t hash64(const std::vector<std::uint8_t> &buf) {
    std::uint64_t h = 1469598103934665603ull;
    for (std::uint8_t b : buf) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace rt

