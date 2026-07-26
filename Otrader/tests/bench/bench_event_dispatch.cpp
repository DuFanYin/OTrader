/**
 * L1.3 — per-event processing cost: PortfolioData::apply_frame(snapshot).
 *
 * apply_frame is the real work a Snapshot event triggers (IV/Greeks for the whole chain). The
 * EventEngine dispatch wrapper around it is a variant-get + pointer deref (negligible), so we
 * benchmark apply_frame directly on a SYNTHETIC fixture sized to a realistic SPXW chain
 * (controlled + repeatable — see benchmarkPlan.md §0.1).
 *
 * KEY FINDING under test (latencyFindings.md F-1): apply_frame spawns hardware_concurrency()
 * std::jthreads PER CALL to parallelize the Greeks. This benchmark quantifies the resulting
 * latency and, critically, its tail/jitter (thread create/join per event).
 */

#include "bench_util.hpp"
#include "black_scholes.hpp"
#include "object.hpp"
#include "portfolio.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using bench::Histogram;
using bench::keep;

namespace {

// Build a PortfolioData with `n_options` synthetic options wired into option_apply_order_, and a
// matching snapshot. Sizes should mirror a real SPXW day (hundreds of strikes across DTEs).
struct Fixture {
    utilities::PortfolioData portfolio{"BENCH"};
    utilities::PortfolioSnapshot snap;
    std::vector<utilities::OptionData> storage; // stable backing for option_apply_order_ ptrs

    explicit Fixture(size_t n_options) {
        storage.resize(n_options);
        portfolio.underlying = std::make_unique<utilities::UnderlyingData>();

        const auto now = std::chrono::system_clock::now();
        const auto expiry = now + std::chrono::hours(24 * 7); // 7 DTE

        portfolio.option_apply_order_.reserve(n_options);
        for (size_t i = 0; i < n_options; ++i) {
            auto& opt = storage[i];
            // Strikes spread around a 5000 spot (SPX-like), alternating call/put.
            opt.strike_price = 4000.0 + static_cast<double>(i % 500) * 4.0;
            opt.option_type = (i % 2 == 0) ? 1 : -1;
            opt.option_expiry = expiry;
            opt.size = 100.0;
            portfolio.option_apply_order_.push_back(&opt);
        }

        snap.portfolio_name = "BENCH";
        snap.datetime = now;
        snap.underlying_bid = 4998.0;
        snap.underlying_ask = 5002.0;
        snap.underlying_last = 5000.0;
        auto fill = [n_options](std::vector<double>& v, double base) {
            v.resize(n_options);
            for (size_t i = 0; i < n_options; ++i) v[i] = base + static_cast<double>(i % 50) * 0.1;
        };
        fill(snap.bid, 10.0);
        fill(snap.ask, 10.4);
        fill(snap.last, 10.2);
        // delta/gamma/... in the snapshot are inputs the engine recomputes; size them to n.
        snap.delta.resize(n_options);
        snap.gamma.resize(n_options);
        snap.theta.resize(n_options);
        snap.vega.resize(n_options);
        snap.iv.resize(n_options);
    }
};

void bench_apply_frame(size_t n_options, const char* label) {
    Fixture fx(n_options);
    // µs-scale → PER_OP timing gives a real per-op tail distribution.
    Histogram h = bench::measure_per_op(
        [&] {
            fx.portfolio.apply_frame(fx.snap);
            keep(fx.portfolio.option_apply_order_[0]->mid_iv);
        },
        /*iters=*/5000, /*warmup=*/200);
    bench::print_row(label, h);
}

} // namespace

int main() {
    bench::print_env();
    std::printf("NOTE: apply_frame spawns hardware_concurrency() jthreads per call (F-1).\n");

    bench::print_header("apply_frame(snapshot) — per Snapshot-event cost, by chain size (ns)");
    bench_apply_frame(64, "apply_frame  64 options");
    bench_apply_frame(256, "apply_frame 256 options");
    bench_apply_frame(1000, "apply_frame 1000 options");
    bench_apply_frame(4000, "apply_frame 4000 options (full SPXW-ish)");

    return 0;
}
