/**
 * Event-replay throughput.
 *
 * "How fast can the engine eat the stream?" Loads a REAL parquet day via the backtest Main
 * (throughput must use real data), which builds the portfolio + precomputes snapshots
 * (one-time setup, excluded from the throughput window). Then replays every snapshot through
 * Portfolio::apply_frame as fast as possible — no wall-clock throttling. Reports events/sec.
 *
 * Parallel scaling: N independent backtest Mains each replay the same file concurrently (mirrors
 * run_backtest_multi's num_engines=4 jthreads), reporting aggregate events/sec and speedup vs 1
 * worker. apply_frame itself forks and joins on the shared thread pool for a large chain; one
 * range at a time, so concurrent replays compute theirs inline: this shows how the two kinds of
 * parallelism mix.
 *
 * Usage: bench_replay <parquet_path>
 */

#include "tests/bench/bench_util.hpp"

#include "adapters/market_data/historical_data.hpp"
#include "tests/lone_backtest.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using bench::clock_ticks;
using bench::ticks_to_ns;

namespace {

// Load via the backtest (builds portfolio + precomputes snapshots). Returns (engine, count).
struct Loaded {
    std::unique_ptr<test::LoneBacktest> engine;
    adapters::HistoricalData* data = nullptr;
    model::Portfolio* portfolio = nullptr;
    size_t count = 0;
    std::unique_ptr<model::PortfolioSnapshot> minute = std::make_unique<model::PortfolioSnapshot>();

    /** Bar i into the portfolio, read in place (the day and the minute outlive the replay). */
    void apply(size_t i) const {
        data->fill(i, *minute);
        portfolio->apply(*minute,
                         std::shared_ptr<const void>(std::shared_ptr<void>{}, minute.get()));
    }
};

Loaded load(const std::string& path) {
    Loaded L;
    L.engine = std::make_unique<test::LoneBacktest>();
    L.data = L.engine->load_backtest_data(path);
    if (L.data && L.data->has_data()) {
        L.data->load_all_chains(); // every chain replayed
        L.portfolio = L.engine->main().portfolios()->find_portfolio(L.data->portfolio_name());
        L.count = L.data->minutes();
    }
    return L;
}

double replay_once(const Loaded& L) {
    uint64_t t0 = clock_ticks();
    for (size_t i = 0; i < L.count; ++i) L.apply(i);
    return ticks_to_ns(clock_ticks() - t0);
}

} // namespace

int main(int argc, char** argv) {
    bench::print_env();
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <parquet_path>\n", argv[0]);
        return 2;
    }
    std::string path = argv[1];

    uint64_t setup0 = clock_ticks();
    Loaded L = load(path);
    double setup_ns = ticks_to_ns(clock_ticks() - setup0);
    if (L.count == 0) {
        std::fprintf(stderr, "failed to load or no snapshots: %s\n", path.c_str());
        return 1;
    }
    std::printf("file          : %s\n", path.c_str());
    std::printf("snapshots     : %zu\n", L.count);
    std::printf("setup (load+precompute): %.1f ms\n", setup_ns / 1e6);

    // ---- Single-thread replay (best of 5) ----
    double best_ns = 1e30;
    for (int r = 0; r < 5; ++r) {
        double e = replay_once(L);
        if (e < best_ns) best_ns = e;
    }
    double single_eps = static_cast<double>(L.count) * 1e9 / best_ns;
    std::printf("\n== single-thread replay ==\n");
    std::printf("replay        : %.2f ms  |  %.0f events/sec  |  %.2f us/event\n", best_ns / 1e6,
                single_eps, best_ns / 1e3 / static_cast<double>(L.count));

    // ---- Parallel scaling: 1/2/4/8 independent engines ----
    std::printf("\n== parallel scaling (N independent replays) ==\n");
    std::printf("%-8s %16s %10s\n", "workers", "events/sec(agg)", "speedup");
    double base_eps = 0.0;
    for (int workers : {1, 2, 4, 8}) {
        std::vector<Loaded> loaded;
        loaded.reserve(workers);
        for (int w = 0; w < workers; ++w) loaded.push_back(load(path));

        std::atomic<bool> go{false};
        std::atomic<int> ready{0};
        uint64_t total = 0;
        for (auto& l : loaded) total += l.count;

        std::vector<std::thread> ths;
        for (int w = 0; w < workers; ++w) {
            ths.emplace_back([&, w] {
                ready.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire)) {}
                auto& l = loaded[w];
                for (size_t i = 0; i < l.count; ++i) l.apply(i);
            });
        }
        while (ready.load(std::memory_order_acquire) < workers) {}
        uint64_t t0 = clock_ticks();
        go.store(true, std::memory_order_release);
        for (auto& t : ths) t.join();
        double elapsed = ticks_to_ns(clock_ticks() - t0);
        double eps = static_cast<double>(total) * 1e9 / elapsed;
        if (workers == 1) base_eps = eps;
        std::printf("%-8d %16.0f %9.2fx\n", workers, eps, base_eps > 0 ? eps / base_eps : 1.0);
    }

    return 0;
}
