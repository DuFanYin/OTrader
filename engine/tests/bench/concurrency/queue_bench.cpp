// Single-producer / single-consumer throughput: lock-free SPSC queue vs a
// mutex-protected ring buffer. The consumer checks that it receives exactly
// 0, 1, 2, ... in order, so a broken queue fails instead of just running fast.
//
//   queue_bench --impl spsc|locked --messages N --capacity C
// Prints one JSON line.

#include "common.hpp"
#include "ring_buffer.hpp"
#include "spsc_queue.hpp"

#include <cstdint>
#include <cstdio>
#include <thread>

template <class Q>
int run(const char* impl, Q& q, uint64_t n) {
    uint64_t bad = 0, sum = 0;
    const double t0 = now_ms();
    std::thread producer([&] {
        for (uint64_t i = 0; i < n; ++i)
            while (!q.push(i)) std::this_thread::yield();
    });
    std::thread consumer([&] {
        uint64_t v;
        for (uint64_t expect = 0; expect < n; ++expect) {
            while (!q.pop(v)) std::this_thread::yield();
            bad += (v != expect);
            sum += v;
        }
    });
    producer.join();
    consumer.join();
    const double ms = now_ms() - t0;
    const bool ok = bad == 0 && sum == n * (n - 1) / 2;
    std::printf("{\"impl\":\"%s\",\"messages\":%llu,\"ms\":%.2f,\"msgs_per_sec\":%.0f,"
                "\"ok\":%s,\"peak_rss_mib\":%.1f}\n",
                impl, static_cast<unsigned long long>(n), ms, n / (ms / 1000.0), ok ? "true" : "false", peak_rss_mib());
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    const std::string impl = arg(argc, argv, "--impl", "spsc");
    const uint64_t n   = std::strtoull(arg(argc, argv, "--messages", "10000000").c_str(), nullptr, 10);
    const unsigned cap = static_cast<unsigned>(std::strtoul(arg(argc, argv, "--capacity", "4096").c_str(), nullptr, 10));
    if (impl == "spsc")   { SpscQueue<uint64_t> q(cap);   return run("spsc", q, n); }
    if (impl == "locked") { LockedQueue<uint64_t> q(cap); return run("locked", q, n); }
    std::fprintf(stderr, "unknown --impl %s\n", impl.c_str());
    return 2;
}
