#pragma once

/**
 * Wakeup: one thread sleeps here until another wakes it or a deadline passes (a Linux futex). A
 * wake costs a system call only while that thread sleeps; otherwise it is a fence and a load. One
 * thread waits at a time.
 *
 * The waiter: wait_until(deadline, has_work) returns at once when has_work(); otherwise it sleeps
 * until a wake() after the work was made visible, or the deadline. A producer: make the work
 * visible (a push, a store), then wake().
 */

#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace utilities {

class Wakeup {
  public:
    using Clock = std::chrono::steady_clock; // CLOCK_MONOTONIC, the futex's clock

    /** After the work is visible: the waiter, asleep or about to sleep, wakes. */
    void wake() {
        std::atomic_thread_fence(std::memory_order_seq_cst); // the work, before reading sleeping_
        if (sleeping_.load(std::memory_order_relaxed)) {
            epoch_.fetch_add(1, std::memory_order_relaxed);
            futex(FUTEX_WAKE_PRIVATE, INT_MAX, nullptr);
        }
    }

    /** Sleep until has_work() or the deadline (Clock::time_point::max(): none). */
    template <typename HasWork> void wait_until(Clock::time_point deadline, HasWork&& has_work) {
        const std::uint32_t epoch = epoch_.load(std::memory_order_relaxed);
        sleeping_.store(true, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst); // sleeping_, before reading the work
        if (!has_work()) {
            if (deadline == Clock::time_point::max()) {
                futex(FUTEX_WAIT_PRIVATE, epoch, nullptr);
            } else {
                const auto since = deadline.time_since_epoch();
                const auto secs = std::chrono::duration_cast<std::chrono::seconds>(since);
                timespec at{};
                at.tv_sec = static_cast<std::time_t>(secs.count());
                at.tv_nsec = static_cast<long>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(since - secs).count());
                // An absolute deadline on CLOCK_MONOTONIC: FUTEX_WAIT_BITSET's.
                futex(FUTEX_WAIT_BITSET_PRIVATE, epoch, &at, FUTEX_BITSET_MATCH_ANY);
            }
        }
        sleeping_.store(false, std::memory_order_relaxed);
    }

  private:
    long futex(int op, std::uint32_t value, const timespec* at, std::uint32_t mask = 0) {
        // A wait returns at once when epoch_ is no longer `value` (a wake came in between).
        return syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(&epoch_), op, value, at, nullptr,
                       mask);
    }

    std::atomic<std::uint32_t> epoch_{0}; // the futex word: a wake moves it on
    std::atomic<bool> sleeping_{false};
    static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));
};

} // namespace utilities
