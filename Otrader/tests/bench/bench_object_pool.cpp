/**
 * L1.2 — object pool latency (ObjectPool<OrderData>).
 *
 * Single-mutex freelist + in_use set (latencyFindings.md F-2). Single-thread acquire/release and
 * the new/delete baseline are ns-scale → BATCH timing. Cross-thread 4-acquire/1-release is
 * µs-scale under mutex contention → PER_OP timing (real per-op tail). OrderData is a representative
 * pooled type (engine pools Event/PortfolioSnapshot/OrderData/TradeData).
 */

#include "bench_util.hpp"
#include "mpsc_ring.hpp"
#include "object.hpp"
#include "object_pool.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using bench::Histogram;
using bench::keep;
using utilities::ObjectPool;
using utilities::OrderData;

namespace {

// BATCH: acquire+release pair, steady state (pool depth oscillates 0/1). Amortized ns/op.
Histogram bench_pool_pair_batch() {
    ObjectPool<OrderData> pool(256);
    return bench::measure_batch(
        [&] {
            OrderData* p = pool.acquire();
            keep(p);
            pool.release(p);
        },
        /*batch=*/2048, /*rounds=*/2000);
}

Histogram bench_new_delete_batch() {
    return bench::measure_batch(
        [] {
            auto* p = new OrderData();
            keep(p);
            delete p;
        },
        /*batch=*/2048, /*rounds=*/2000);
}

// Cross-thread: 4 producers acquire + hand off (via engine's own MpscRing so the queue doesn't
// perturb the pool measurement), 1 consumer releases. PER_OP timing on both sides.
void bench_pool_cross() {
    ObjectPool<OrderData> pool(2048);
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 1000000;
    utilities::MpscRing<OrderData*, 4096> handoff;

    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> released{0};
    std::vector<Histogram> acq_recs(kProducers);
    Histogram rel_rec;

    std::thread consumer([&] {
        OrderData* p = nullptr;
        while (!stop.load(std::memory_order_acquire)) {
            while (handoff.try_pop(p)) {
                uint64_t t0 = bench::clock_ticks();
                pool.release(p);
                uint64_t t1 = bench::clock_ticks();
                rel_rec.record_ns(bench::ticks_to_ns(t1 - t0));
                released.fetch_add(1, std::memory_order_relaxed);
            }
        }
        while (handoff.try_pop(p)) {
            pool.release(p);
            released.fetch_add(1, std::memory_order_relaxed);
        }
    });

    std::vector<std::thread> producers;
    for (int pi = 0; pi < kProducers; ++pi) {
        producers.emplace_back([&, pi] {
            while (!go.load(std::memory_order_acquire)) {}
            Histogram& rec = acq_recs[pi];
            for (int i = 0; i < kPerProducer; ++i) {
                uint64_t t0 = bench::clock_ticks();
                OrderData* p = pool.acquire();
                uint64_t t1 = bench::clock_ticks();
                rec.record_ns(bench::ticks_to_ns(t1 - t0));
                if (p)
                    while (!handoff.try_push(p)) {}
            }
        });
    }

    go.store(true, std::memory_order_release);
    for (auto& t : producers) t.join();
    while (released.load(std::memory_order_acquire) <
           static_cast<uint64_t>(kProducers) * kPerProducer)
        std::this_thread::yield();
    stop.store(true, std::memory_order_release);
    consumer.join();

    bench::print_header("ObjectPool<OrderData> — cross-thread (4 acquire, 1 release), PER_OP");
    for (int pi = 0; pi < kProducers; ++pi) {
        char name[48];
        std::snprintf(name, sizeof(name), "acquire[producer %d]", pi);
        bench::print_row(name, acq_recs[pi]);
    }
    bench::print_row("release (consumer)", rel_rec);
}

} // namespace

int main() {
    bench::print_env();

    Histogram pool_pair = bench_pool_pair_batch();
    Histogram nd = bench_new_delete_batch();
    bench::print_header("ObjectPool<OrderData> vs new/delete — single thread, BATCH (ns/op)");
    bench::print_row_batch("pool acquire+release", pool_pair);
    bench::print_row_batch("new+delete OrderData", nd);

    bench_pool_cross();
    return 0;
}
