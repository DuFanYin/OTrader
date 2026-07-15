#pragma once

/**
 * SPSC (single producer, single consumer) lock-free ring buffer.
 * T must be trivially copyable (e.g. raw pointers). Capacity must be a power of 2.
 * Producer: try_push(T). Consumer: try_pop(T&). One producer thread, one consumer thread.
 */

#include <atomic>
#include <cstddef>

namespace utilities {

template <typename T, size_t Capacity> class SpscRing {
  public:
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "SpscRing: Capacity must be a power of 2 and >= 2");

    SpscRing() = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    /** Producer: push one item. Returns false if ring is full. */
    bool try_push(T value) {
        size_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_.load(std::memory_order_acquire) >= Capacity) {
            return false;
        }
        buffer_[t & mask_] = value;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    /** Consumer: pop one item into out. Returns false if ring is empty. */
    bool try_pop(T& out) {
        size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        out = buffer_[h & mask_];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    /** Approximate number of items (for debugging). */
    size_t size_approx() const {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_acquire);
        return t - h;
    }

    bool empty() const { return size_approx() == 0; }
    static constexpr size_t capacity() { return Capacity; }

  private:
    static constexpr size_t mask_ = Capacity - 1;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    T buffer_[Capacity]{};
};

} // namespace utilities
