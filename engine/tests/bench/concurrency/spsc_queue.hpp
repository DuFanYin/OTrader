#pragma once

// Bounded single-producer / single-consumer lock-free queue.
//
// Ported from Option-Pricing (Queue/LockFreeQueue_multi.hpp), the atomic
// acquire/release variant. Changes from the original:
//   - SPSC only. The original test drove it with 2 producers and 2 consumers,
//     which races on head/tail; it only counted messages, so lost or duplicated
//     messages went unnoticed.
//   - Heap storage only. The shared-memory mode kept head/tail in process
//     memory, so it never worked across processes.
//   - Power-of-two capacity (mask instead of modulo) and head/tail on separate
//     cache lines so producer and consumer do not false-share.
//   - The full fences were redundant with the release/acquire pair and are gone.
// The original Queue/LockFreeQueue.hpp (volatile indices, no atomics) is not
// ported: it is a data race under the C++ memory model and can reorder on ARM.

#include <atomic>
#include <cstddef>
#include <memory>
#include <new>

template <class T>
class SpscQueue {
public:
    explicit SpscQueue(size_t min_capacity) {
        size_t cap = 2;
        while (cap < min_capacity) cap <<= 1;
        mask_ = cap - 1;
        buf_  = std::make_unique<T[]>(cap);
    }

    // Producer thread only.
    bool push(const T& v) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        const size_t next = (tail + 1) & mask_;
        if (next == head_.load(std::memory_order_acquire)) return false;   // full
        buf_[tail] = v;
        tail_.store(next, std::memory_order_release);                       // publish the slot
        return true;
    }

    // Consumer thread only.
    bool pop(T& v) {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) return false;    // empty
        v = buf_[head];
        head_.store((head + 1) & mask_, std::memory_order_release);         // free the slot
        return true;
    }

    size_t capacity() const { return mask_; }   // one slot is kept empty

private:
    static constexpr size_t kLine = 64;
    alignas(kLine) std::atomic<size_t> head_{0};
    alignas(kLine) std::atomic<size_t> tail_{0};
    alignas(kLine) size_t mask_ = 0;
    std::unique_ptr<T[]> buf_;
};
