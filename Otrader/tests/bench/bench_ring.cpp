/**
 * L1.1 — lock-free ring latency (MpscRing / SpscRing).
 *
 * Ops move raw pointers, so latency is payload-independent (dummy pointers). Real hot-path
 * capacities: main ring = MpscRing<Event*,512>, strategy ring = SpscRing<Event*,256>.
 *
 * Uncontended push/pop are sub-clock-resolution (< ~42 ns/tick), so they use BATCH timing
 * (ns/op over many rounds — the number is the throughput-limited per-op cost, the spread is
 * jitter). The contended MPSC case uses per-thread PER_OP timing since pushes are µs-scale there.
 */

#include "bench_util.hpp"
#include "mpsc_ring.hpp"
#include "spsc_ring.hpp"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using bench::Histogram;
using bench::keep;

namespace {

constexpr size_t kMainCap = 512;     // matches live EventEngine queue_ring_
constexpr size_t kStrategyCap = 256; // matches live EventEngine strategy_ring_

int g_dummy = 0;
int* const kPtr = &g_dummy;

// BATCH: sustained push+pop pairs. One pair keeps the ring at steady depth 0/1 so try_push never
// fails; measures the amortized per-op cost of the ring machinery.
template <typename Ring> Histogram bench_pushpop_batch() {
    Ring ring;
    int* out = nullptr;
    return bench::measure_batch(
        [&] {
            ring.try_push(kPtr);
            ring.try_pop(out);
            keep(out);
        },
        /*batch=*/4096, /*rounds=*/2000);
}

// Contended MPSC: 4 producers hammer try_push, 1 consumer drains. Per-push PER_OP timing +
// aggregate throughput — the real live shape (4 producers → main ring).
void bench_mpsc_contended() {
    utilities::MpscRing<int*, kMainCap> ring;
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 2000000;
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> pushed{0};
    std::vector<Histogram> recs(kProducers);

    std::thread consumer([&] {
        int* out = nullptr;
        while (!stop.load(std::memory_order_acquire))
            while (ring.try_pop(out)) keep(out);
        while (ring.try_pop(out)) keep(out);
    });

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            while (!go.load(std::memory_order_acquire)) {}
            Histogram& rec = recs[p];
            uint64_t local = 0;
            for (int i = 0; i < kPerProducer; ++i) {
                uint64_t t0 = bench::clock_ticks();
                bool ok = ring.try_push(kPtr);
                uint64_t t1 = bench::clock_ticks();
                rec.record_ns(bench::ticks_to_ns(t1 - t0));
                if (ok) ++local;
            }
            pushed.fetch_add(local, std::memory_order_relaxed);
        });
    }

    uint64_t start = bench::clock_ticks();
    go.store(true, std::memory_order_release);
    for (auto& t : producers) t.join();
    double elapsed_ns = bench::ticks_to_ns(bench::clock_ticks() - start);
    stop.store(true, std::memory_order_release);
    consumer.join();

    bench::print_header("MpscRing<int*,512> — contended (4 producers, 1 consumer), PER_OP");
    for (int p = 0; p < kProducers; ++p) {
        char name[48];
        std::snprintf(name, sizeof(name), "push[producer %d]", p);
        bench::print_row(name, recs[p]);
    }
    bench::print_throughput("push (all producers)", pushed.load(), elapsed_ns);
}

} // namespace

int main() {
    bench::print_env();

    Histogram mpsc = bench_pushpop_batch<utilities::MpscRing<int*, kMainCap>>();
    Histogram spsc = bench_pushpop_batch<utilities::SpscRing<int*, kStrategyCap>>();

    bench::print_header("Ring push+pop — uncontended, BATCH (ns/op amortized)");
    bench::print_row_batch("MpscRing<int*,512> push+pop", mpsc);
    bench::print_row_batch("SpscRing<int*,256> push+pop", spsc);

    bench_mpsc_contended();
    return 0;
}
