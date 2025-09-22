#pragma once
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__linux__) && defined(SIM_USE_NUMA)
#include <numa.h>
#endif

#if defined(__linux__)
#include <sys/mman.h> // mlock, madvise, munlock
#include <unistd.h>   // sysconf
#endif

// Optional toggles (define before including if you want them ON):
// #define RT_ARENA_PRETOUCH          1
// #define RT_ARENA_MLOCK             1
// #define RT_ARENA_MADVISE_HUGEPAGE  1

namespace rt {

// ----------------- utils -----------------
static inline std::size_t normalize_align(std::size_t a) noexcept {
  if (a < alignof(std::max_align_t))
    a = alignof(std::max_align_t);
  if ((a & (a - 1)) != 0) { // round up to pow2
    std::size_t x = 1;
    while (x < a)
      x <<= 1;
    a = x;
  }
  return a;
}

static inline std::size_t os_page_size() noexcept {
#if defined(__linux__)
  long pz = ::sysconf(_SC_PAGESIZE);
  return pz > 0 ? static_cast<std::size_t>(pz) : static_cast<std::size_t>(4096);
#else
  return static_cast<std::size_t>(4096);
#endif
}

// -------------- FrameArena --------------
// Strict bump allocator: O(1) allocate, per-frame reset, NO fallback heap.
// Optional: page pre-touch, mlock, hugepage advice at init.
class FrameArena {
public:
  enum class AllocationStatus { Ok, Degraded, Failed };

  using FallbackProvider = void *(*)(std::size_t);

  static void setFallbackProvider(FallbackProvider fn) noexcept {
    fallbackProvider_.store(fn, std::memory_order_release);
  }

  static void resetFallbackProvider() noexcept {
    fallbackProvider_.store(nullptr, std::memory_order_release);
  }

  AllocationStatus lastStatus() const noexcept { return lastStatus_; }
  bool degraded() const noexcept { return degradeSticky_; }
  void clearDegraded() noexcept { degradeSticky_ = false; }
  std::size_t fallbackBytes() const noexcept { return fallbackBytes_; }

  FrameArena() = default;
  explicit FrameArena(std::size_t capacityBytes,
                      std::size_t alignment = 64, int numaNode = -1) {
    configure(capacityBytes, alignment, numaNode);
  }

  FrameArena(const FrameArena &) = delete;
  FrameArena &operator=(const FrameArena &) = delete;

  // Movable (for container init-time only)
  FrameArena(FrameArena &&o) noexcept
      : buffer_(o.buffer_), capacity_(o.capacity_), head_(o.head_),
        alignment_(o.alignment_), degradeActive_(o.degradeActive_),
        degradeSticky_(o.degradeSticky_), lastStatus_(o.lastStatus_),
        fallbackAllocs_(std::move(o.fallbackAllocs_)),
        fallbackBytes_(o.fallbackBytes_)
#if defined(__linux__) && defined(SIM_USE_NUMA)
        , numaNode_(o.numaNode_)
#endif
#if defined(__linux__)
        , pageSize_(o.pageSize_)
#endif
  {
    o.buffer_ = nullptr;
    o.capacity_ = 0;
    o.head_ = 0;
    o.degradeActive_ = false;
    o.degradeSticky_ = false;
    o.lastStatus_ = AllocationStatus::Ok;
    o.fallbackBytes_ = 0;
    o.fallbackAllocs_.clear();
#if defined(__linux__) && defined(SIM_USE_NUMA)
    o.numaNode_ = -1;
#endif
  }

  FrameArena &operator=(FrameArena &&o) noexcept {
    if (this != &o) {
      release_();
      buffer_ = o.buffer_;
      o.buffer_ = nullptr;
      capacity_ = o.capacity_;
      o.capacity_ = 0;
      head_ = o.head_;
      o.head_ = 0;
      alignment_ = o.alignment_;
      degradeActive_ = o.degradeActive_;
      degradeSticky_ = o.degradeSticky_;
      lastStatus_ = o.lastStatus_;
      fallbackAllocs_ = std::move(o.fallbackAllocs_);
      fallbackBytes_ = o.fallbackBytes_;
#if defined(__linux__) && defined(SIM_USE_NUMA)
      numaNode_ = o.numaNode_;
      o.numaNode_ = -1;
#endif
#if defined(__linux__)
      pageSize_ = o.pageSize_;
#endif
      o.degradeActive_ = false;
      o.degradeSticky_ = false;
      o.lastStatus_ = AllocationStatus::Ok;
      o.fallbackBytes_ = 0;
      o.fallbackAllocs_.clear();
    }
    return *this;
  }

  ~FrameArena() { release_(); }

  void configure(std::size_t capacityBytes, std::size_t alignment = 64,
                 int numaNode = -1) {
    alignment = normalize_align(alignment);
    if (buffer_ && capacity_ == capacityBytes && alignment_ == alignment
#if defined(__linux__) && defined(SIM_USE_NUMA)
        && numaNode_ == numaNode
#endif
    ) {
      head_ = 0;
#if !(defined(__linux__) && defined(SIM_USE_NUMA))
      (void)numaNode;
#endif
      return;
    }
    alignment_ = alignment;
#if defined(__linux__) && defined(SIM_USE_NUMA)
    numaNode_ = numaNode;
#else
    (void)numaNode;
#endif
    reserve_(capacityBytes);
  }

  void reset() noexcept {
    head_ = 0;
    releaseFallback_();
    degradeActive_ = false;
    lastStatus_ = AllocationStatus::Ok;
  }
  std::size_t capacity() const noexcept { return capacity_; }
  std::size_t used() const noexcept { return head_; }
  std::size_t remaining() const noexcept { return capacity_ - head_; }
  std::size_t alignment() const noexcept { return alignment_; }

  void *allocate(std::size_t size,
                 std::size_t align = alignof(std::max_align_t))
#if defined(NDEBUG)
      noexcept
#endif
  {
    void *result = nullptr;
    allocate(size, &result, align);
    return result;
  }

  AllocationStatus allocate(std::size_t size, void **out,
                            std::size_t align = alignof(std::max_align_t))
#if defined(NDEBUG)
      noexcept
#endif
  {
    return allocateImpl_(size, align, out);
  }

  template <typename T>
  T *allocateArray(std::size_t count)
#if defined(NDEBUG)
      noexcept
#endif
  {
    static_assert(!std::is_const<T>::value, "cannot allocate const");
    void *p = allocate(sizeof(T) * count, alignof(T));
    return reinterpret_cast<T *>(p);
  }

  struct Marker {
    std::size_t head;
  };
  Marker mark() const noexcept { return Marker{head_}; }
  void rewind(Marker m) noexcept {
    assert(m.head <= head_);
    head_ = m.head;
  }

private:
  AllocationStatus allocateImpl_(std::size_t size, std::size_t align,
                                 void **out)
#if defined(NDEBUG)
      noexcept
#endif
  {
    if (out)
      *out = nullptr;

    align = normalize_align(align);
    lastStatus_ = AllocationStatus::Ok;

    if (!degradeActive_) {
      const auto base = reinterpret_cast<std::uintptr_t>(buffer_);
      const std::uintptr_t p = base + head_;
      const std::uintptr_t aligned =
          (p + (align - 1)) & ~(static_cast<std::uintptr_t>(align) - 1);
      const std::size_t alignedHead = static_cast<std::size_t>(aligned - base);

      if (alignedHead <= capacity_ && size <= capacity_ - alignedHead) {
        head_ = alignedHead + size;
        if (out)
          *out = reinterpret_cast<void *>(aligned);
        lastStatus_ = AllocationStatus::Ok;
        return lastStatus_;
      }

      const std::size_t avail =
          alignedHead <= capacity_ ? (capacity_ - alignedHead) : 0u;
      std::fprintf(stderr,
                   "[RtArena] overflow: req=%zu avail=%zu cap=%zu off=%zu\n",
                   size, avail, capacity_, alignedHead);
      std::fflush(stderr);
#if defined(NDEBUG)
      degradeActive_ = true;
      degradeSticky_ = true;
#else
      throw std::bad_alloc();
#endif
    }

    void *fallback = allocateFallback_(size, align);
    if (fallback) {
      if (out)
        *out = fallback;
      lastStatus_ = AllocationStatus::Degraded;
    } else {
      lastStatus_ = AllocationStatus::Failed;
    }
    return lastStatus_;
  }

  void *allocateFallback_(std::size_t size, std::size_t align) noexcept {
    const std::size_t requested = size;
    if (size == 0)
      size = align;
    if (align > std::numeric_limits<std::size_t>::max() - sizeof(void *))
      return nullptr;
    const std::size_t padding = align + sizeof(void *);
    if (size > std::numeric_limits<std::size_t>::max() - padding)
      return nullptr;
    const std::size_t total = size + padding;
    auto provider = fallbackProvider_.load(std::memory_order_acquire);
    void *raw = provider ? provider(total) : std::malloc(total);
    if (!raw)
      return nullptr;
    auto rawAddr = reinterpret_cast<std::uintptr_t>(raw) + sizeof(void *);
    auto alignedAddr =
        (rawAddr + (align - 1)) & ~(static_cast<std::uintptr_t>(align) - 1);
    auto alignedPtr = reinterpret_cast<void *>(alignedAddr);
    auto meta = reinterpret_cast<void **>(alignedAddr - sizeof(void *));
    *meta = raw;
    fallbackAllocs_.push_back(FallbackAlloc{alignedPtr, raw, requested});
    fallbackBytes_ += requested;
    return alignedPtr;
  }

  void releaseFallback_() noexcept {
    for (auto &alloc : fallbackAllocs_) {
      if (alloc.raw)
        std::free(alloc.raw);
    }
    fallbackAllocs_.clear();
    fallbackBytes_ = 0;
  }

  void reserve_(std::size_t cap) {
    release_();
    capacity_ = cap;
    if (capacity_) {
#if defined(__linux__) && defined(SIM_USE_NUMA)
      if (numaNode_ >= 0 && numa_available() != -1) {
        buffer_ =
            static_cast<std::byte *>(numa_alloc_onnode(capacity_, numaNode_));
      } else
#endif
      {
        buffer_ = static_cast<std::byte *>(
            ::operator new(capacity_, std::align_val_t(alignment_)));
      }
#if defined(__linux__)
      pageSize_ = os_page_size();
#endif
#if RT_ARENA_MADVISE_HUGEPAGE
#if defined(__linux__)
      (void)::madvise(buffer_, capacity_, MADV_HUGEPAGE);
#endif
#endif
#if RT_ARENA_MLOCK
#if defined(__linux__)
      (void)::mlock(buffer_, capacity_);
#endif
#endif
#if RT_ARENA_PRETOUCH
      pre_touch_();
#endif
    }
    head_ = 0;
    degradeActive_ = false;
    lastStatus_ = AllocationStatus::Ok;
    degradeSticky_ = false;
  }

  void release_() noexcept {
    releaseFallback_();
    if (buffer_) {
#if RT_ARENA_MLOCK
#if defined(__linux__)
      (void)::munlock(buffer_, capacity_);
#endif
#endif
#if defined(__linux__) && defined(SIM_USE_NUMA)
      if (numaNode_ >= 0 && numa_available() != -1) {
        numa_free(buffer_, capacity_);
      } else
#endif
      {
        ::operator delete(buffer_, std::align_val_t(alignment_));
      }
      buffer_ = nullptr;
    }
    capacity_ = 0;
    head_ = 0;
    degradeActive_ = false;
    degradeSticky_ = false;
    lastStatus_ = AllocationStatus::Ok;
  }

  void pre_touch_() noexcept {
    const std::size_t page =
#if defined(__linux__)
        pageSize_ ? pageSize_ : os_page_size();
#else
        os_page_size();
#endif
    for (std::size_t i = 0; i < capacity_; i += page)
      buffer_[i] = std::byte{0};
    if (capacity_)
      buffer_[capacity_ - 1] = std::byte{0};
  }

  struct FallbackAlloc {
    void *aligned;
    void *raw;
    std::size_t size;
  };

  bool degradeActive_ = false;
  bool degradeSticky_ = false;
  AllocationStatus lastStatus_ = AllocationStatus::Ok;
  std::vector<FallbackAlloc> fallbackAllocs_{};
  std::size_t fallbackBytes_ = 0;

  std::byte *buffer_ = nullptr;
  std::size_t capacity_ = 0;
  std::size_t head_ = 0;
  std::size_t alignment_ = 64;
#if defined(__linux__) && defined(SIM_USE_NUMA)
  int numaNode_ = -1;
#endif
#if defined(__linux__)
  std::size_t pageSize_ = 4096;
#endif

  static std::atomic<FallbackProvider> fallbackProvider_;
};

inline std::atomic<FrameArena::FallbackProvider> FrameArena::fallbackProvider_{
    nullptr};

// -------------- FrameArenaPool --------------
class FrameArenaPool {
public:
  FrameArenaPool(std::size_t threads,
                 std::size_t perThreadCapacityBytes = (1u << 20),
                 std::size_t baseAlignment = 64,
                 const std::vector<int> &nodes = {})
      : arenas_(threads), nodes_(nodes), perThreadCapacityBytes_(perThreadCapacityBytes),
        baseAlignment_(baseAlignment), nextIndex_(0) {
    nodes_.resize(threads, -1);

    static_assert(std::is_move_constructible<FrameArena>::value,
                  "FrameArena must be move-constructible");
  }

  void beginFrame() noexcept {
    for (auto &a : arenas_)
      a.reset();
  }

  // Deterministic binding (preferred). Call once from thread i during init.
  void bindCurrentThread(std::size_t i) noexcept {
    assert(i < arenas_.size());
    tlsIndex_ = i;
    if (perThreadCapacityBytes_) {
      int node = nodeForIndex(i);
      arenas_[i].configure(perThreadCapacityBytes_, baseAlignment_, node);
    }
  }

  // Non-deterministic first-touch assignment (optional).
  std::size_t claimNextSlot() noexcept {
    const std::size_t n = arenas_.size();
    assert(n && "FrameArenaPool constructed with zero arenas");
    return nextIndex_.fetch_add(1, std::memory_order_relaxed) % n;
  }

  FrameArena &tls() noexcept {
    assert(tlsIndex_ != static_cast<std::size_t>(-1) &&
           "Thread not bound; call bindCurrentThread(i)");
    return arenas_[tlsIndex_];
  }

  std::size_t threads() const noexcept { return arenas_.size(); }
  const FrameArena &arena(std::size_t i) const noexcept { return arenas_[i]; }
  FrameArena &arena(std::size_t i) noexcept { return arenas_[i]; }
  int node(std::size_t i) const noexcept {
    return nodeForIndex(i);
  }

private:
  int nodeForIndex(std::size_t i) const noexcept {
    return (i < nodes_.size()) ? nodes_[i] : -1;
  }

  std::vector<FrameArena> arenas_;
  std::vector<int> nodes_;
  std::size_t perThreadCapacityBytes_ = 0;
  std::size_t baseAlignment_ = 64;
  std::atomic<std::size_t> nextIndex_;
  static thread_local std::size_t tlsIndex_;
};

// Header-only definition (C++17 inline variable)
inline thread_local std::size_t FrameArenaPool::tlsIndex_ =
    static_cast<std::size_t>(-1);

} // namespace rt
