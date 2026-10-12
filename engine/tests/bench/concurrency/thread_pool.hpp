#pragma once

// Fixed-size thread pool with a mutex + condition-variable task queue, ported
// from Option-Pricing (monteCarlo/monteCarlo_pool_mem.cpp). Adds parallel_for,
// which blocks until every index has run, so it can drive lsm_detail::run.

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool {
public:
    explicit ThreadPool(unsigned n_threads) {
        for (unsigned i = 0; i < n_threads; ++i) {
            workers_.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock lk(m_);
                        cv_.wait(lk, [this] { return done_ || !tasks_.empty(); });
                        if (done_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    task();
                }
            });
        }
    }

    ~ThreadPool() {
        { std::lock_guard lk(m_); done_ = true; }
        cv_.notify_all();
        for (auto& w : workers_) w.join();
    }

    void submit(std::function<void()> f) {
        { std::lock_guard lk(m_); tasks_.push(std::move(f)); }
        cv_.notify_one();
    }

    // Run fn(i) for i in [0, n) on the pool, one task per index; wait for all.
    template <class F>
    void parallel_for(int n, F&& fn) {
        std::mutex done_m;
        std::condition_variable done_cv;
        int remaining = n;
        for (int i = 0; i < n; ++i) {
            submit([&, i] {
                fn(i);
                std::lock_guard lk(done_m);
                if (--remaining == 0) done_cv.notify_one();
            });
        }
        std::unique_lock lk(done_m);
        done_cv.wait(lk, [&] { return remaining == 0; });
    }

    unsigned size() const { return static_cast<unsigned>(workers_.size()); }

private:
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex m_;
    std::condition_variable cv_;
    bool done_ = false;
};
