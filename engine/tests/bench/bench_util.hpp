#pragma once

/**
 * Latency-benchmark harness (header-only). Built for HFT-style measurement: report the whole
 * distribution (p50/p99/p99.9/max), not the mean.
 *
 * Clock: the CPU's counter, cheap and the same on every core: rdtsc on x86 (an invariant TSC,
 * its rate measured once against steady_clock), cntvct_el0 on arm64 (24 MHz on Apple silicon:
 * 41.67 ns a tick). Without one, steady_clock. A single op shorter than a few ticks cannot be
 * timed on its own, so two modes:
 *
 *   - BATCH   : time a batch of N identical ops with ONE clock read pair, report total/N. This is
 *               the only way to see ops near the clock's resolution (ring push/pop, pool acquire).
 *               Loses the per-op tail, but gives a trustworthy mean/op + batch-to-batch spread.
 *   - PER_OP  : time each op on its own. For ops well above the resolution (apply_frame, a path
 *               through the engine's threads). Gives the real per-op tail.
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace bench {

// ----------------------------- clock (TSC) -----------------------------

#if defined(__aarch64__)
inline uint64_t tsc_ticks() {
    uint64_t v;
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
inline double tsc_ns_per_tick() {
    uint64_t f;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? 1e9 / static_cast<double>(f) : 0.0;
}
#elif defined(__x86_64__)
inline uint64_t tsc_ticks() { return __rdtsc(); }
/** rdtsc's rate, measured once against steady_clock; 0 (steady_clock instead) unless the TSC is
 * invariant (constant_tsc and nonstop_tsc: the same rate on every core, in every power state). */
inline double tsc_ns_per_tick() {
    static const double ns_per_tick = [] {
        std::ifstream cpuinfo("/proc/cpuinfo");
        for (std::string line; std::getline(cpuinfo, line);) {
            if (!line.starts_with("flags")) {
                continue;
            }
            if (line.find(" constant_tsc") == std::string::npos ||
                line.find(" nonstop_tsc") == std::string::npos) {
                return 0.0;
            }
            const auto c0 = std::chrono::steady_clock::now();
            const uint64_t t0 = __rdtsc();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const auto c1 = std::chrono::steady_clock::now();
            const uint64_t t1 = __rdtsc();
            return std::chrono::duration<double, std::nano>(c1 - c0).count() /
                   static_cast<double>(t1 - t0);
        }
        return 0.0;
    }();
    return ns_per_tick;
}
#else
inline uint64_t tsc_ticks() { return 0; }
inline double tsc_ns_per_tick() { return 0.0; }
#endif

// Unified ns clock: TSC when its frequency is known, else steady_clock.
inline bool tsc_usable() { return tsc_ns_per_tick() > 0.0; }

inline uint64_t clock_ticks() {
    if (tsc_usable()) return tsc_ticks();
    return static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}
inline double ticks_to_ns(uint64_t dt) {
    double npt = tsc_ns_per_tick();
    return npt > 0.0 ? static_cast<double>(dt) * npt : static_cast<double>(dt); // steady_clock is ns
}

/** Prevent the compiler from optimizing away a computed value. */
template <typename T> inline void keep(T&& x) { asm volatile("" : : "r,m"(x) : "memory"); }
inline void clobber() { asm volatile("" : : : "memory"); }

/** A spin-wait's pause: the core's other thread runs meanwhile. */
inline void cpu_relax() {
#if defined(__x86_64__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

/** Spin (no sleep, no system call) for ns. */
inline void spin_for_ns(double ns) {
    const uint64_t start = clock_ticks();
    while (ticks_to_ns(clock_ticks() - start) < ns) cpu_relax();
}

// ----------------------------- histogram recorder -----------------------------

/**
 * HdrHistogram-style recorder: fixed-memory log-linear buckets, O(1) record, any percentile.
 * Range: 1 ns .. ~17 s. Precision ~1% (SUB_BITS mantissa bits per power-of-two).
 */
class Histogram {
  public:
    void record_ns(double ns) {
        uint64_t v = ns < 1.0 ? 0 : static_cast<uint64_t>(ns);
        ++total_;
        sum_ += ns;
        if (v > max_) max_ = v;
        if (v < min_) min_ = v;
        ++buckets_[bucket_index(v)];
    }
    uint64_t count() const { return total_; }
    double mean() const { return total_ ? sum_ / static_cast<double>(total_) : 0.0; }
    uint64_t min() const { return total_ ? min_ : 0; }
    uint64_t max() const { return max_; }

    uint64_t pct(double p) const {
        if (!total_) return 0;
        uint64_t target = static_cast<uint64_t>(std::ceil(p / 100.0 * static_cast<double>(total_)));
        if (target == 0) target = 1;
        uint64_t cum = 0;
        for (size_t i = 0; i < kBuckets; ++i) {
            cum += buckets_[i];
            if (cum >= target) return bucket_value(i);
        }
        return max_;
    }

  private:
    // log-linear: for value v, high bit h = bit_width(v); index = h*SUB + top SUB_BITS mantissa.
    static constexpr int kSubBits = 6;            // 64 sub-buckets per octave (~1.5% precision)
    static constexpr int kSub = 1 << kSubBits;    // 64
    static constexpr int kOctaves = 45;           // covers up to ~2^45 ns ≈ 9.8 hours
    static constexpr size_t kBuckets = static_cast<size_t>(kOctaves) * kSub;

    static int bit_width(uint64_t v) {
        int b = 0;
        while (v) { v >>= 1; ++b; }
        return b; // bit_width(0)=0, bit_width(1)=1
    }
    static size_t bucket_index(uint64_t v) {
        if (v < static_cast<uint64_t>(kSub)) return v; // linear region [0, 64)
        int h = bit_width(v) - 1;                      // top set bit position
        int shift = h - kSubBits;
        uint64_t mant = (v >> shift) & (kSub - 1);
        size_t idx = static_cast<size_t>(h) * kSub + mant;
        return idx < kBuckets ? idx : kBuckets - 1;
    }
    static uint64_t bucket_value(size_t idx) {
        if (idx < static_cast<size_t>(kSub)) return static_cast<uint64_t>(idx);
        int h = static_cast<int>(idx / kSub);
        uint64_t mant = idx % kSub;
        int shift = h - kSubBits;
        return (static_cast<uint64_t>(mant) | static_cast<uint64_t>(kSub)) << shift;
    }

    std::array<uint32_t, kBuckets> buckets_{};
    uint64_t total_ = 0;
    uint64_t min_ = ~0ULL;
    uint64_t max_ = 0;
    double sum_ = 0.0;
};

// ----------------------------- measurement modes -----------------------------

/**
 * BATCH: run `op` `batch` times under one clock read, repeat `rounds` rounds. Records ns/op
 * (batch mean) per round into a Histogram → distribution is over ROUNDS, not per-op. Use for
 * ns-scale ops that the clock can't resolve individually.
 */
template <typename Op>
Histogram measure_batch(Op&& op, uint64_t batch, uint64_t rounds, uint64_t warmup_rounds = 3) {
    for (uint64_t w = 0; w < warmup_rounds; ++w)
        for (uint64_t i = 0; i < batch; ++i) op();
    Histogram h;
    for (uint64_t r = 0; r < rounds; ++r) {
        uint64_t t0 = clock_ticks();
        for (uint64_t i = 0; i < batch; ++i) op();
        uint64_t t1 = clock_ticks();
        h.record_ns(ticks_to_ns(t1 - t0) / static_cast<double>(batch));
    }
    return h;
}

/**
 * PER_OP: time each of `iters` ops individually. Correct for µs+ ops. `warmup` ops discarded.
 */
template <typename Op>
Histogram measure_per_op(Op&& op, uint64_t iters, uint64_t warmup = 200) {
    for (uint64_t w = 0; w < warmup; ++w) op();
    Histogram h;
    for (uint64_t i = 0; i < iters; ++i) {
        uint64_t t0 = clock_ticks();
        op();
        uint64_t t1 = clock_ticks();
        h.record_ns(ticks_to_ns(t1 - t0));
    }
    return h;
}

// ----------------------------- multi-round stability -----------------------------

/** Coefficient of variation (%) of a set of per-round values — batch mode returns one value/round. */
struct RunStats {
    double median_ns;
    double min_ns;
    double cv_pct; // stddev/mean across rounds
};

inline RunStats summarize_runs(std::vector<double> per_round_ns) {
    RunStats s{0, 0, 0};
    if (per_round_ns.empty()) return s;
    std::sort(per_round_ns.begin(), per_round_ns.end());
    s.min_ns = per_round_ns.front();
    s.median_ns = per_round_ns[per_round_ns.size() / 2];
    double mean = 0;
    for (double v : per_round_ns) mean += v;
    mean /= static_cast<double>(per_round_ns.size());
    double var = 0;
    for (double v : per_round_ns) var += (v - mean) * (v - mean);
    var /= static_cast<double>(per_round_ns.size());
    s.cv_pct = mean > 0 ? 100.0 * std::sqrt(var) / mean : 0.0;
    return s;
}

// ----------------------------- reporting -----------------------------

inline void print_header(const char* title) {
    std::printf("\n== %s ==\n", title);
    std::printf("%-40s %10s %10s %10s %10s %10s %10s\n", "operation (ns)", "p50", "p99", "p99.9",
                "max", "mean", "min");
}

inline void print_row(const char* name, const Histogram& h) {
    std::printf("%-40s %10llu %10llu %10llu %10llu %10.0f %10llu\n", name,
                (unsigned long long)h.pct(50), (unsigned long long)h.pct(99),
                (unsigned long long)h.pct(99.9), (unsigned long long)h.max(), h.mean(),
                (unsigned long long)h.min());
}

inline void print_row_batch(const char* name, const Histogram& per_round) {
    // For batch mode: the distribution is over rounds; p50 = typical ns/op, spread = jitter.
    std::printf("%-40s %10llu %10llu %10llu %10llu %10.0f %10llu   (ns/op, over rounds)\n", name,
                (unsigned long long)per_round.pct(50), (unsigned long long)per_round.pct(99),
                (unsigned long long)per_round.pct(99.9), (unsigned long long)per_round.max(),
                per_round.mean(), (unsigned long long)per_round.min());
}

inline void print_throughput(const char* name, uint64_t ops, double elapsed_ns) {
    double mops = elapsed_ns > 0 ? static_cast<double>(ops) * 1e9 / elapsed_ns / 1e6 : 0.0;
    std::printf("%-40s %12llu ops  %10.2f Mops/s\n", name, (unsigned long long)ops, mops);
}

// ----------------------------- environment capture -----------------------------

inline std::string cpu_brand() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    for (std::string line; std::getline(cpuinfo, line);) {
        if (line.starts_with("model name") && line.find(':') != std::string::npos) {
            return line.substr(line.find(':') + 2);
        }
    }
    return "unknown";
}

inline void print_env() {
    std::printf("--------------------------------------------------------------------------------\n");
    std::printf("cpu           : %s\n", cpu_brand().c_str());
    std::printf("cores (hw)    : %u\n", std::thread::hardware_concurrency());
#if defined(__x86_64__)
    const char* counter = "TSC rdtsc";
#else
    const char* counter = "TSC cntvct_el0";
#endif
    std::printf("clock         : %s (%.3f ns/tick)\n", tsc_usable() ? counter : "steady_clock",
                tsc_usable() ? tsc_ns_per_tick() : 1.0);
    std::printf("build         : -O3 -DNDEBUG (Release)\n");
    std::printf("note          : ns-scale ops use BATCH timing (clock resolution ~%.1f ns);\n",
                tsc_usable() ? tsc_ns_per_tick() : 20.0);
    std::printf("                us+ ops use PER_OP timing. Distributions: PER_OP = per-op tail,\n");
    std::printf("                BATCH = ns/op across rounds (jitter, not per-op tail).\n");
    std::printf("--------------------------------------------------------------------------------\n");
}

} // namespace bench
