#pragma once

/**
 * Persistent thread pool for data-parallel compute (per-option IV/Greeks in
 * PortfolioData::apply_frame), without a lock.
 *
 * Why this exists: apply_frame used to spawn hardware_concurrency() std::jthreads PER CALL, so the
 * thread create/join cost (~110 µs for 64 options, tail into tens of ms) dwarfed the actual work. A persistent pool creates its workers
 * ONCE.
 *
 * parallel_for is a fork/join: the caller publishes the range (a generation counter the workers
 * sleep on: one futex wake for all of them), runs a share itself, and waits for the workers'
 * shares (spinning a little, then on a futex). Each of the n workers and the caller takes one
 * contiguous share. One range at a time: a caller that finds the pool busy with another thread's
 * range runs its own inline, as it does a range at or below inline_threshold.
 *
 * Scope: one engine-level SHARED pool for the whole process (see shared_pool()).
 */

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

namespace utilities {

class ThreadPool {
  public:
    /** n_workers: 0 for one fewer than the hardware threads (the caller takes a share too). */
    explicit ThreadPool(unsigned int n_workers = 0) {
        if (n_workers == 0) {
            const unsigned int hw = std::thread::hardware_concurrency();
            n_workers = hw > 1 ? hw - 1 : 1;
        }
        n_workers_ = n_workers;
        workers_.reserve(n_workers);
        for (unsigned int w = 0; w < n_workers; ++w) {
            workers_.emplace_back([this, w] { worker_loop(w); });
        }
    }

    ~ThreadPool() {
        stop_.store(true, std::memory_order_relaxed);
        generation_.fetch_add(1, std::memory_order_release);
        generation_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    unsigned int size() const { return n_workers_; }

    /**
     * Run fn(i) for i in [begin, end) across the pool, blocking until all complete. Ranges at or
     * below `inline_threshold` run inline on the caller (no worker involvement), as does a range
     * while the pool works on another thread's.
     */
    template <typename Fn>
    void parallel_for(std::size_t begin, std::size_t end, Fn&& fn,
                      std::size_t inline_threshold = 256) {
        const std::size_t total = end > begin ? end - begin : 0;
        if (total == 0) {
            return;
        }
        if (total <= inline_threshold || n_workers_ <= 1 ||
            busy_.exchange(true, std::memory_order_acquire)) {
            for (std::size_t i = begin; i < end; ++i) {
                fn(i);
            }
            return;
        }
        const std::size_t shares = std::size_t{n_workers_} + 1;
        begin_ = begin;
        end_ = end;
        chunk_ = (total + shares - 1) / shares;
        run_ = &run_range<std::remove_reference_t<Fn>>;
        fn_ = const_cast<void*>(static_cast<const void*>(std::addressof(fn)));
        done_.store(0, std::memory_order_relaxed);
        generation_.fetch_add(1, std::memory_order_release); // the range is out
        generation_.notify_all();
        run_share(n_workers_); // the caller's: the last

        std::uint32_t done = done_.load(std::memory_order_acquire);
        for (int spin = 0; done < n_workers_ && spin < kSpins; ++spin) {
            pause();
            done = done_.load(std::memory_order_acquire);
        }
        while (done < n_workers_) {
            done_.wait(done, std::memory_order_acquire);
            done = done_.load(std::memory_order_acquire);
        }
        busy_.store(false, std::memory_order_release);
    }

  private:
    /** The caller's spins for the workers before it sleeps (a share takes tens of µs). */
    static constexpr int kSpins = 2000;

    static void pause() {
#if defined(__x86_64__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield");
#endif
    }

    template <typename F> static void run_range(void* fn, std::size_t s, std::size_t e) {
        F& f = *static_cast<F*>(fn);
        for (std::size_t i = s; i < e; ++i) {
            f(i);
        }
    }

    /** Share `share` of the range out now (none when the range is shorter). */
    void run_share(std::size_t share) {
        const std::size_t s = begin_ + share * chunk_;
        if (s < end_) {
            run_(fn_, s, std::min(s + chunk_, end_));
        }
    }

    void worker_loop(unsigned int w) {
        std::uint32_t seen = 0;
        for (;;) {
            generation_.wait(seen, std::memory_order_acquire); // until the next range is out
            seen = generation_.load(std::memory_order_acquire);
            if (stop_.load(std::memory_order_relaxed)) {
                return;
            }
            run_share(w);
            // The range's fields are not read after this: the next caller may rewrite them.
            if (done_.fetch_add(1, std::memory_order_acq_rel) + 1 == n_workers_) {
                done_.notify_one();
            }
        }
    }

    unsigned int n_workers_ = 1;
    std::vector<std::thread> workers_;

    // The range out (written by its caller before generation_ moves on, read by the workers after).
    std::size_t begin_ = 0;
    std::size_t end_ = 0;
    std::size_t chunk_ = 0;
    void (*run_)(void*, std::size_t, std::size_t) = nullptr;
    void* fn_ = nullptr;

    alignas(64) std::atomic<std::uint32_t> generation_{0}; // a range out moves it on
    alignas(64) std::atomic<std::uint32_t> done_{0};       // the workers done with it
    std::atomic<bool> busy_{false};                        // a caller's range is out
    std::atomic<bool> stop_{false};
};

/**
 * Process-wide shared pool (Meyers singleton: thread-safe init, ordered destruction). This is the
 * "engine-level shared pool" — all portfolios/engines use this one instance rather than spawning
 * their own threads.
 */
inline ThreadPool& shared_pool() {
    static ThreadPool pool;
    return pool;
}

} // namespace utilities
