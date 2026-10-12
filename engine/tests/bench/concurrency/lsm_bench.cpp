// The engine's Longstaff-Schwartz code (engine/quant/pricing/lsm_impl.h)
// under three executors: single thread, OpenMP, and the ported thread pool.
// Each chunk owns its RNG stream and partial sums, so all three must produce
// bit-identical prices; the run script checks that.
//
//   lsm_bench --backend serial|omp|pool --threads T --paths P --steps N --repeat R
// Prints one JSON line. Run one backend per process so peak RSS is per backend.

#include "common.hpp"
#include "thread_pool.hpp"
#include "quant/pricing/lsm_impl.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <omp.h>
#include <vector>

int main(int argc, char** argv) {
    const std::string backend = arg(argc, argv, "--backend", "omp");
    const int threads = std::atoi(arg(argc, argv, "--threads", "0").c_str());
    const int paths   = std::atoi(arg(argc, argv, "--paths", "200000").c_str());
    const int steps   = std::atoi(arg(argc, argv, "--steps", "50").c_str());
    const int repeat  = std::max(1, std::atoi(arg(argc, argv, "--repeat", "5").c_str()));
    const unsigned n_threads = threads > 0 ? threads : std::max(1u, std::thread::hardware_concurrency());

    std::unique_ptr<ThreadPool> pool;
    if (backend == "pool") pool = std::make_unique<ThreadPool>(n_threads);
    if (backend == "omp") omp_set_num_threads(static_cast<int>(n_threads));

    const auto serial_for = [](int n, auto&& fn) { for (int i = 0; i < n; ++i) fn(i); };
    const auto omp_for = [](int n, auto&& fn) {
#pragma omp parallel for schedule(static)
        for (int i = 0; i < n; ++i) fn(i);
    };
    const auto pool_for = [&](int n, auto&& fn) { pool->parallel_for(n, fn); };

    // American put, S = K = 100, r = 5%, sigma = 20%, T = 1.
    const auto price_once = [&] {
        constexpr double S = 100, K = 100, r = 0.05, q = 0.0, vol = 0.2, T = 1.0;
        if (backend == "serial") return quant::lsm_detail::run(S, K, r, q, vol, T, paths, steps, false, 42, true, serial_for);
        if (backend == "pool")   return quant::lsm_detail::run(S, K, r, q, vol, T, paths, steps, false, 42, true, pool_for);
        return quant::lsm_detail::run(S, K, r, q, vol, T, paths, steps, false, 42, true, omp_for);
    };
    if (backend != "serial" && backend != "omp" && backend != "pool") {
        std::fprintf(stderr, "unknown --backend %s\n", backend.c_str());
        return 2;
    }

    std::vector<double> times;
    quant::lsm_detail::Output out;
    for (int k = 0; k < repeat; ++k) {
        const double t0 = now_ms();
        out = price_once();
        times.push_back(now_ms() - t0);
    }
    std::sort(times.begin(), times.end());
    std::printf("{\"backend\":\"%s\",\"threads\":%u,\"paths\":%d,\"steps\":%d,\"repeat\":%d,"
                "\"best_ms\":%.2f,\"median_ms\":%.2f,\"price\":%.17g,\"std_error\":%.17g,\"peak_rss_mib\":%.1f}\n",
                backend.c_str(), backend == "serial" ? 1u : n_threads, out.n_paths, steps, repeat,
                times.front(), times[times.size() / 2], out.price, out.std_error, peak_rss_mib());
    return 0;
}
