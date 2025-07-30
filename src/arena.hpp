#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cassert>
#include <new>

/* ----------------------------------------------------------
   Very small “frame arena”  –  bump‑pointer allocator.
   • 64‑byte aligned base so SIMD loads are always safe
   • reset() every frame; no individual frees
   ---------------------------------------------------------- */
class FrameArena
{
public:
    explicit FrameArena(std::size_t capBytes = 4 * 1024 * 1024)  // 4 MB default
    {
        base_ = static_cast<std::uint8_t*>(std::aligned_alloc(64, capBytes));
        assert(base_ && "arena alloc failed");
        capacity_ = capBytes;
        reset();
    }
    ~FrameArena() { std::free(base_); }

    void  reset()                     { offset_ = 0; }
    std::size_t capacity() const      { return capacity_; }
    std::size_t used() const          { return offset_; }

    void* alloc(std::size_t bytes, std::size_t align = 64)
    {
        std::size_t p = reinterpret_cast<std::size_t>(base_) + offset_;
        std::size_t aligned = (p + align - 1) & ~(align - 1);
        std::size_t newOff  = aligned - reinterpret_cast<std::size_t>(base_) + bytes;
        assert(newOff <= capacity_ && "FrameArena overflow");
        offset_ = newOff;
        return reinterpret_cast<void*>(aligned);
    }

    template<typename T>
    T* make(std::size_t count = 1)
    {
        void* mem = alloc(sizeof(T) * count, alignof(T));
        return new (mem) T[count];
    }

private:
    std::uint8_t*  base_     = nullptr;
    std::size_t    capacity_ = 0;
    std::size_t    offset_   = 0;
};
