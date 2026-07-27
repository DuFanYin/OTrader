/**
 * L5 — classic HFT metrics (throughput / capacity / determinism), broker-free.
 *
 * L1/L2 measure primitive latency & replay throughput. L5 measures the engine as a trading system:
 * how many orders/sec it sustains, its inbound message-rate capacity before it drops, the queueing
 * delay events suffer under load, and jitter (tail/median) — the determinism metric HFT cares about
 * more than absolute latency. See benchmarkPlan §4b.
 *
 * All measured without a broker:
 *   - throughput      : orders/sec through the strategy-facing submit path (backtest send_order).
 *   - message-rate    : the live engine's inbound queue is MpscRing<Event*,512> + CV notify, with a
 *                        300-iter spin then DROP on full (engine_event.cpp push_acquired_to_main_ring).
 *                        We reproduce that exact enqueue discipline against a real MpscRing<...,512>
 *                        with a consumer draining at a fixed service rate, and sweep producer load to
 *                        find the drop knee (backpressure curve).
 *   - queueing delay  : time an item waits in the ring (enqueue TSC → dequeue TSC) under load — the
 *                        dominant real-world latency source once the engine is saturated.
 *   - jitter          : p99.9 / p50 ratio of enqueue latency — 1.0 = perfectly deterministic.
 */

#include "bench_util.hpp"

#include "engine_main.hpp"    // backtest::MainEngine (throughput)
#include "mpsc_ring.hpp"      // same ring the live EventEngine uses
#include "object.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using bench::clock_ticks;
using bench::Histogram;
using bench::keep;
using bench::ticks_to_ns;

namespace {

// ---------------------------------------------------------------------------
// 1) Throughput: orders/sec through the strategy-facing submit path.
// ---------------------------------------------------------------------------
void bench_throughput() {
    backtest::MainEngine engine;
    int next_id = 0;
    engine.set_order_executor(
        [&](const utilities::OrderRequest&) -> std::string { return "O" + std::to_string(++next_id); });

    utilities::OrderRequest req;
    req.symbol = "SPXW  250804C05000000";
    req.direction = utilities::Direction::LONG;
    req.type = utilities::OrderType::LIMIT;
    req.volume = 1;
    req.price = 10.0;

    const uint64_t N = 500000;
    for (uint64_t i = 0; i < 5000; ++i) keep(engine.send_order("s", req)); // warmup
    uint64_t t0 = clock_ticks();
    for (uint64_t i = 0; i < N; ++i) keep(engine.send_order("s", req));
    double ns = ticks_to_ns(clock_ticks() - t0);
    double ops = static_cast<double>(N) * 1e9 / ns;
    std::printf("submit throughput (1 thread) : %.0f orders/sec  (%.0f ns/order)\n", ops,
                ns / static_cast<double>(N));
}

// ---------------------------------------------------------------------------
// Shared enqueue discipline: mirror engine_event.cpp push_acquired_to_main_ring exactly —
// try_push; else spin up to 300 yields; else DROP. Returns true if enqueued, false if dropped.
// ---------------------------------------------------------------------------
template <typename Ring, typename T> bool engine_style_push(Ring& ring, T v) {
    if (ring.try_push(v)) return true;
    for (int spin = 0; spin < 300 && !ring.try_push(v); ++spin) std::this_thread::yield();
    return ring.try_push(v);
}

// A queued item carries its enqueue timestamp so the consumer can measure queueing delay.
struct Item {
    uint64_t enq_tick;
    uint32_t seq;
};

// ---------------------------------------------------------------------------
// 2+3+4) Message-rate capacity, backpressure drop curve, queueing delay, jitter.
//
// Model: an UNPACED producer offers items as fast as it can into the live engine's exact enqueue
// discipline (MpscRing<512>, 300-spin, then drop). The single consumer pops as fast as it can but
// pays a fixed per-item service cost `service_ns` (busy-wait, no syscall) standing in for the
// engine's real per-event work. Drop happens when the producer outruns the consumer — the drop%
// vs service_ns curve is the backpressure characteristic. service_ns=0 measures the ring's raw
// single-consumer drain ceiling. Queueing delay = enqueue TSC → dequeue TSC.
// ---------------------------------------------------------------------------
struct LoadResult {
    double offered_rate;   // items/sec producer actually offered
    double drain_rate;     // items/sec consumer actually drained (enqueued/elapsed)
    uint64_t enqueued;
    uint64_t dropped;
    double drop_pct;
    Histogram enq_ns;      // enqueue-call latency (incl. spin)
    Histogram queue_ns;    // time spent waiting in ring
};

LoadResult run_load(uint64_t total, double service_ns) {
    static constexpr size_t kCap = 512; // == live EventEngine kMainRingCap
    utilities::MpscRing<Item*, kCap> ring;
    std::vector<Item> storage(total);

    std::atomic<bool> done{false};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> drained{0};
    Histogram queue_ns;
    double svc_ticks = service_ns > 0 ? service_ns / bench::tsc_ns_per_tick() : 0;
    std::thread consumer([&] {
        Item* p = nullptr;
        while (!done.load(std::memory_order_acquire) || !ring.empty()) {
            if (ring.try_pop(p) && p != nullptr) {
                uint64_t now = clock_ticks();
                queue_ns.record_ns(ticks_to_ns(now - p->enq_tick));
                drained.fetch_add(1, std::memory_order_relaxed);
                if (svc_ticks > 0) {
                    uint64_t until = now + static_cast<uint64_t>(svc_ticks);
                    while (clock_ticks() < until) {}
                }
            } else {
                std::this_thread::yield();
            }
        }
    });

    Histogram enq_ns;
    uint64_t start = clock_ticks();
    for (uint64_t i = 0; i < total; ++i) { // unpaced: offer as fast as possible
        Item* it = &storage[i];
        it->seq = static_cast<uint32_t>(i);
        it->enq_tick = clock_ticks();
        uint64_t e0 = it->enq_tick;
        bool ok = engine_style_push(ring, it);
        enq_ns.record_ns(ticks_to_ns(clock_ticks() - e0));
        if (!ok) dropped.fetch_add(1, std::memory_order_relaxed);
    }
    double offered_ns = ticks_to_ns(clock_ticks() - start);
    done.store(true, std::memory_order_release);
    consumer.join();

    LoadResult r;
    r.offered_rate = static_cast<double>(total) * 1e9 / offered_ns;
    r.dropped = dropped.load();
    r.enqueued = total - r.dropped;
    r.drain_rate = static_cast<double>(drained.load()) * 1e9 / offered_ns;
    r.drop_pct = 100.0 * static_cast<double>(r.dropped) / static_cast<double>(total);
    r.enq_ns = enq_ns;
    r.queue_ns = queue_ns;
    return r;
}

void bench_message_rate() {
    const uint64_t total = 300000;

    // Raw drain ceiling (svc=0): report best-of-N. This row is scheduling-sensitive — when the
    // consumer keeps up the 512-slot ring never fills (0 drop, tens of M/s); a single run can be
    // dominated by a slow thread wakeup, so we take the best drain rate over a few rounds.
    double best_drain = 0;
    uint64_t best_drop = total;
    for (int r = 0; r < 5; ++r) {
        LoadResult lr = run_load(total, 0.0);
        if (lr.drain_rate > best_drain) { best_drain = lr.drain_rate; best_drop = lr.dropped; }
    }
    std::printf("\n== message-rate capacity + backpressure (ring cap 512, unpaced producer) ==\n");
    std::printf("raw drain ceiling (consumer keeps up, best of 5): %.0f items/sec, %llu dropped\n",
                best_drain, (unsigned long long)best_drop);
    std::printf("backpressure — consumer given fixed per-item work (drop when producer outruns it):\n");
    std::printf("%-14s %14s %14s %10s %10s %14s\n", "svc/item(ns)", "offered/sec", "drained/sec",
                "dropped", "drop%", "queue p99(ns)");
    // Slower consumer → deterministic backpressure (these rows are stable run-to-run).
    for (double svc : {500.0, 2000.0, 5000.0}) {
        LoadResult r = run_load(total, svc);
        std::printf("%-14.0f %14.0f %14.0f %10llu %9.1f%% %14llu\n", svc, r.offered_rate,
                    r.drain_rate, (unsigned long long)r.dropped, r.drop_pct,
                    (unsigned long long)r.queue_ns.pct(99));
    }
}

void bench_jitter() {
    // Jitter = tail/median of enqueue latency at a moderate service cost (below saturation).
    std::printf("\n== jitter (enqueue + queueing determinism, consumer 500ns/item) ==\n");
    LoadResult r = run_load(300000, 500.0);
    uint64_t p50 = r.enq_ns.pct(50), p999 = r.enq_ns.pct(99.9), qp50 = r.queue_ns.pct(50),
             qp999 = r.queue_ns.pct(99.9);
    std::printf("enqueue    p50=%llu ns  p99.9=%llu ns  jitter(p99.9/p50)=%.1fx\n",
                (unsigned long long)p50, (unsigned long long)p999,
                p50 ? static_cast<double>(p999) / static_cast<double>(p50) : 0.0);
    std::printf("queueing   p50=%llu ns  p99.9=%llu ns  jitter(p99.9/p50)=%.1fx\n",
                (unsigned long long)qp50, (unsigned long long)qp999,
                qp50 ? static_cast<double>(qp999) / static_cast<double>(qp50) : 0.0);
}

} // namespace

int main() {
    bench::print_env();
    if (!bench::tsc_usable()) {
        std::printf("WARNING: TSC frequency unknown; pacing/queueing figures unreliable on this host.\n");
    }

    bench::print_header("throughput (orders/sec)");
    bench_throughput();

    bench_message_rate();
    bench_jitter();

    return 0;
}
