#pragma once
#include <atomic>
#include <cassert>
#include <cstddef>
#include <functional>
#include <thread>
#include <vector>

namespace simcore {

// Simple hazard pointer implementation for lock-free structures.
// One hazard pointer per thread; retired nodes are reclaimed when unprotected.

struct HazardRecord {
  std::atomic<void *> pointer{nullptr};
  HazardRecord *next{nullptr};
};

inline std::atomic<HazardRecord *> global_hazard_head{nullptr};

inline HazardRecord *acquire_hazard() {
  thread_local HazardRecord *rec = nullptr;
  if (rec)
    return rec;
  rec = new HazardRecord();
  HazardRecord *old = global_hazard_head.load(std::memory_order_acquire);
  do {
    rec->next = old;
  } while (!global_hazard_head.compare_exchange_weak(
      old, rec, std::memory_order_release, std::memory_order_relaxed));
  return rec;
}

class HazardGuard {
public:
  HazardGuard() : rec_(acquire_hazard()) {}
  ~HazardGuard() { clear(); }

  template <typename T> void protect(std::atomic<T *> &src) {
    void *p = src.load(std::memory_order_acquire);
    rec_->pointer.store(p, std::memory_order_release);
    // Ensure pointer is still current
    while (p != src.load(std::memory_order_acquire)) {
      p = src.load(std::memory_order_acquire);
      rec_->pointer.store(p, std::memory_order_release);
    }
  }

  void clear() { rec_->pointer.store(nullptr, std::memory_order_release); }

private:
  HazardRecord *rec_;
};

struct RetiredNode {
  void *ptr;
  void (*deleter)(void *);
};

inline void default_delete(void *p) { delete static_cast<char *>(p); }

inline std::vector<RetiredNode> &retired_list() {
  thread_local std::vector<RetiredNode> list;
  return list;
}

inline void scan() {
  // Gather all hazard pointers
  std::vector<void *> hazards;
  for (HazardRecord *r = global_hazard_head.load(std::memory_order_acquire); r;
       r = r->next) {
    hazards.push_back(r->pointer.load(std::memory_order_acquire));
  }
  auto &list = retired_list();
  auto it = list.begin();
  while (it != list.end()) {
    void *p = it->ptr;
    bool protected_ptr = false;
    for (void *h : hazards) {
      if (h == p) {
        protected_ptr = true;
        break;
      }
    }
    if (!protected_ptr) {
      it->deleter(p);
      it = list.erase(it);
    } else {
      ++it;
    }
  }
}

inline void retire(void *p, void (*deleter)(void *) = default_delete) {
  auto &list = retired_list();
  list.push_back({p, deleter});
  if (list.size() >= 16)
    scan();
}

} // namespace simcore
