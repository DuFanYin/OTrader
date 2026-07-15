#pragma once

/**
 * MPSC (multi producer, single consumer) lock-free bounded ring buffer.
 * T must be trivially copyable (e.g. raw pointers). Capacity must be a power of 2.
 * Multiple producers: try_push(T). Single consumer: try_pop(T&).
 */

#include <atomic>
#include <cstddef>

namespace utilities {

template <typename T, size_t Capacity> class MpscRing {
  public:
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "MpscRing: Capacity must be a power of 2 and >= 2");

    MpscRing() {
        for (size_t i = 0; i < Capacity; ++i) {
            seq_[i].store(i, std::memory_order_relaxed);
        }
    }
    MpscRing(const MpscRing&) = delete;
    MpscRing& operator=(const MpscRing&) = delete;

    /** Producer (any thread): push one item. Returns false if ring is full. */
    bool try_push(T value) {
        size_t w = write_pos_.load(std::memory_order_relaxed);
        for (;;) {
            size_t r = read_pos_.load(std::memory_order_acquire);
            if (w - r >= Capacity) {
                return false;
            }
            if (write_pos_.compare_exchange_weak(w, w + 1, std::memory_order_acq_rel)) {
                break;
            }
        }
        size_t i = w & mask_;
        while (seq_[i].load(std::memory_order_acquire) != w) {
            /* spin until slot is ready for this ticket */
        }
        buffer_[i] = value;
        seq_[i].store(w + Capacity, std::memory_order_release);
        return true;
    }

    /** Consumer (single thread only): pop one item into out. Returns false if empty. */
    bool try_pop(T& out) {
        size_t r = read_pos_.load(std::memory_order_relaxed);
        size_t i = r & mask_;
        if (seq_[i].load(std::memory_order_acquire) != r + Capacity) {
            return false;
        }
        out = buffer_[i];
        seq_[i].store(r + Capacity, std::memory_order_release);
        read_pos_.store(r + 1, std::memory_order_release);
        return true;
    }

    /** Approximate number of items (for debugging). */
    size_t size_approx() const {
        size_t w = write_pos_.load(std::memory_order_acquire);
        size_t r = read_pos_.load(std::memory_order_acquire);
        return w - r;
    }

    bool empty() const { return size_approx() == 0; }
    static constexpr size_t capacity() { return Capacity; }

  private:
    static constexpr size_t mask_ = Capacity - 1;
    alignas(64) std::atomic<size_t> write_pos_{0};
    alignas(64) std::atomic<size_t> read_pos_{0};
    T buffer_[Capacity]{};
    alignas(64) std::atomic<size_t> seq_[Capacity];
};

} // namespace utilities
