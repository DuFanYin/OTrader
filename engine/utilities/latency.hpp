#pragma once

/**
 * Latency on the hot paths, measured as the engine runs: tsc_now() reads the CPU's time-stamp
 * counter (a few nanoseconds; steady_clock where there is none), tsc_ns() turns a difference into
 * nanoseconds (calibrated once against steady_clock), and LatencyHistogram keeps a distribution of
 * them: one writer, any number of readers, nothing locked or allocated.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <thread>
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

namespace utilities {

/** Now, in TSC ticks (steady_clock nanoseconds without a TSC). */
inline std::uint64_t tsc_now() {
#if defined(__x86_64__) || defined(__i386__)
    return __rdtsc();
#else
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
#endif
}

/** Nanoseconds a TSC tick: measured once, the first time asked (10 ms against steady_clock). */
inline double tsc_ns_per_tick() {
    static const double ns_per_tick = [] {
#if defined(__x86_64__) || defined(__i386__)
        const auto t0 = std::chrono::steady_clock::now();
        const std::uint64_t c0 = tsc_now();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto t1 = std::chrono::steady_clock::now();
        const std::uint64_t c1 = tsc_now();
        const double ns = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        return c1 > c0 ? ns / static_cast<double>(c1 - c0) : 1.0;
#else
        return 1.0;
#endif
    }();
    return ns_per_tick;
}

/** The nanoseconds from TSC reading `from` to `to`. */
inline std::uint64_t tsc_ns(std::uint64_t from, std::uint64_t to) {
    return to > from ? static_cast<std::uint64_t>(static_cast<double>(to - from) * tsc_ns_per_tick())
                     : 0;
}

/**
 * A distribution of durations in nanoseconds: buckets a quarter of a power of two wide (so a
 * percentile is within about 19% of the truth), up to 2^40 ns. record() from one thread; summary()
 * from any (relaxed counts: a reading taken while it records may be one sample off).
 */
class LatencyHistogram {
  public:
    struct Summary {
        std::uint64_t count = 0;
        std::uint64_t p50 = 0;
        std::uint64_t p99 = 0;
        std::uint64_t p999 = 0;
        std::uint64_t max = 0;
    };

    void record(std::uint64_t ns) {
        const std::size_t b = bucket(ns);
        counts_[b].store(counts_[b].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        count_.store(count_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        if (ns > max_.load(std::memory_order_relaxed)) {
            max_.store(ns, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] Summary summary() const {
        Summary s;
        s.count = count_.load(std::memory_order_relaxed);
        s.max = max_.load(std::memory_order_relaxed);
        if (s.count == 0) {
            return s;
        }
        auto at = [&](double q) -> std::uint64_t {
            const auto rank = static_cast<std::uint64_t>(q * static_cast<double>(s.count - 1)) + 1;
            std::uint64_t seen = 0;
            for (std::size_t b = 0; b < kBuckets; ++b) {
                seen += counts_[b].load(std::memory_order_relaxed);
                if (seen >= rank) {
                    return std::min(upper(b), s.max); // a bucket's top, never past the largest seen
                }
            }
            return s.max;
        };
        s.p50 = at(0.50);
        s.p99 = at(0.99);
        s.p999 = at(0.999);
        return s;
    }

    /** One line: what it is, then the count and p50 / p99 / p99.9 / max in microseconds. */
    [[nodiscard]] std::string line(std::string_view what) const {
        const Summary s = summary();
        auto us = [](std::uint64_t ns) { return static_cast<double>(ns) / 1000.0; };
        return std::format("{}: {} samples, p50 {:.1f} us, p99 {:.1f} us, p99.9 {:.1f} us, max {:.1f} us",
                           what, s.count, us(s.p50), us(s.p99), us(s.p999), us(s.max));
    }

  private:
    static constexpr std::size_t kSub = 4;            // buckets per power of two
    static constexpr std::size_t kBuckets = 41 * kSub; // up to 2^40 ns (about 18 minutes)

    static std::size_t bucket(std::uint64_t ns) {
        if (ns < kSub) {
            return static_cast<std::size_t>(ns);
        }
        const auto power = static_cast<std::size_t>(std::bit_width(ns) - 1); // 2^power <= ns
        const auto sub = static_cast<std::size_t>((ns >> (power - 2)) & (kSub - 1));
        const std::size_t b = power * kSub + sub;
        return b < kBuckets ? b : kBuckets - 1;
    }
    /** The largest value a bucket holds: what a percentile in it reads as. */
    static std::uint64_t upper(std::size_t b) {
        if (b < kSub) {
            return b;
        }
        const std::size_t power = b / kSub;
        const std::size_t sub = b % kSub;
        return ((std::uint64_t{kSub} + sub + 1) << (power - 2)) - 1;
    }

    std::array<std::atomic<std::uint64_t>, kBuckets> counts_{};
    std::atomic<std::uint64_t> count_{0};
    std::atomic<std::uint64_t> max_{0};
};

} // namespace utilities
