#pragma once

/**
 * Minimal persistent thread pool for data-parallel compute (e.g. per-option IV/Greeks in
 * PortfolioData::apply_frame).
 *
 * Why this exists: apply_frame used to spawn hardware_concurrency() std::jthreads PER CALL, so the
 * thread create/join cost (~110 µs for 64 options, tail into tens of ms — see
 * local/latencyFindings.md F-1) dwarfed the actual work and, worse, its tail latency exploded. A
 * persistent pool creates its workers ONCE; apply_frame then just submits a chunked range and
 * blocks until done — keeping the parallel speedup on large chains while removing the per-event
 * thread-spawn cost and jitter.
 *
 * Scope: one engine-level SHARED pool for the whole process (see shared_pool()). All portfolios /
 * engines share it rather than each owning threads.
 *
 * parallel_for is BLOCKING (bulk-synchronous): it returns only after every chunk has run. Small
 * ranges run inline on the calling thread (no worker wakeup) so tiny chains stay cheap.
 */

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace utilities {

class ThreadPool {
  public:
    explicit ThreadPool(unsigned int n_workers = 0) {
        if (n_workers == 0) n_workers = std::max(1U, std::thread::hardware_concurrency());
        n_workers_ = n_workers;
        workers_.reserve(n_workers);
        for (unsigned int i = 0; i < n_workers; ++i)
            workers_.emplace_back([this] { worker_loop(); });
    }

    ~ThreadPool() {
        {
            std::lock_guard lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_)
            if (t.joinable()) t.join();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    unsigned int size() const { return n_workers_; }

    /**
     * Run fn(i) for i in [begin, end) across the pool, blocking until all complete. Work is split
     * into `n_workers` contiguous chunks. Ranges at/below `inline_threshold` run inline on the
     * caller (no worker involvement) — avoids wakeup cost for tiny chains.
     */
    template <typename Fn>
    void parallel_for(std::size_t begin, std::size_t end, Fn&& fn,
                      std::size_t inline_threshold = 256) {
        const std::size_t total = end > begin ? end - begin : 0;
        if (total == 0) return;
        if (total <= inline_threshold || n_workers_ <= 1) {
            for (std::size_t i = begin; i < end; ++i) fn(i);
            return;
        }

        const std::size_t chunk = (total + n_workers_ - 1) / n_workers_;
        std::atomic<std::size_t> remaining{0};
        // Reference-capture fn; workers run it directly (same thread-safety contract the old
        // per-call jthread version had: each index is independent, no shared writes to the same i).
        std::function<void()> tasks_done_signal;

        {
            std::unique_lock lk(mu_);
            for (unsigned int w = 0; w < n_workers_; ++w) {
                const std::size_t s = begin + static_cast<std::size_t>(w) * chunk;
                if (s >= end) break;
                const std::size_t e = std::min(s + chunk, end);
                remaining.fetch_add(1, std::memory_order_relaxed);
                queue_.push_back([&fn, s, e, &remaining, this] {
                    for (std::size_t i = s; i < e; ++i) fn(i);
                    if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                        std::lock_guard dlk(done_mu_);
                        done_cv_.notify_all();
                    }
                });
            }
            cv_.notify_all();
        }

        // Block until all submitted chunks finished.
        std::unique_lock dlk(done_mu_);
        done_cv_.wait(dlk, [&] { return remaining.load(std::memory_order_acquire) == 0; });
    }

  private:
    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();
        }
    }

    unsigned int n_workers_ = 1;
    std::vector<std::thread> workers_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    bool stop_ = false;

    std::mutex done_mu_;
    std::condition_variable done_cv_;
};

/**
 * Process-wide shared pool (Meyers singleton: thread-safe init, ordered destruction). This is the
 * "engine-level shared pool" — all portfolios/engines use this one instance rather than spawning
 * their own threads. Future engine threading (execution thread, per-strategy) can converge here.
 */
inline ThreadPool& shared_pool() {
    static ThreadPool pool;
    return pool;
}

} // namespace utilities
