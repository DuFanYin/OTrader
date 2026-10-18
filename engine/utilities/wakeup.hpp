#pragma once

/**
 * Wakeup: one thread sleeps here until another wakes it or a deadline passes (a Linux futex). A
 * wake costs a system call only while that thread sleeps; otherwise it is a fence and a load. One
 * thread waits at a time.
 *
 * The waiter: wait_until(deadline, has_work) returns at once when has_work(); otherwise it sleeps
 * until a wake() after the work was made visible, or the deadline. A producer: make the work
 * visible (a push, a store), then wake().
 *
 * set_spin(d): before sleeping, the waiter polls has_work() for up to d, not marked asleep, so
 * work that comes meanwhile costs neither side a system call (a wake() is then a fence and a
 * load). The latency of a hand-off drops from a futex wake-up (about 5 us) to well under one, for
 * a core kept busy while it spins. Zero (the default): sleep at once.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <ctime>
#include <linux/futex.h>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif
#include <sys/syscall.h>
#include <unistd.h>

namespace utilities {

/** A hint to the core that this is a spin loop: a sibling hyperthread gets the cycles. */
inline void cpu_pause() {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

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

    /** How long the waiter polls before it sleeps (zero: it sleeps at once). Set before it waits. */
    void set_spin(std::chrono::nanoseconds spin) { spin_ = spin; }

    /** Sleep until has_work() or the deadline (Clock::time_point::max(): none). */
    template <typename HasWork> void wait_until(Clock::time_point deadline, HasWork&& has_work) {
        if (spin_ > std::chrono::nanoseconds::zero()) {
            const Clock::time_point until = std::min(deadline, Clock::now() + spin_);
            for (unsigned polls = 0;; ++polls) {
                if (has_work()) {
                    return;
                }
                // The clock every 64 polls: a few hundred ns between looks at it.
                if ((polls & 63U) == 63U && Clock::now() >= until) {
                    break;
                }
                cpu_pause();
            }
        }
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
    std::chrono::nanoseconds spin_{0};
    static_assert(sizeof(std::atomic<std::uint32_t>) == sizeof(std::uint32_t));
};

} // namespace utilities
