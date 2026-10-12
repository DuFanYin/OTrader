/**
 * What a snapshot costs, by chain size, on a SYNTHETIC fixture sized to a realistic SPXW chain
 * (controlled + repeatable):
 *   - price_snapshot: forwards, IV and Greeks for the whole chain, where live pricing runs (the
 *     market data thread; a backtest before its run);
 *   - apply_frame of a priced snapshot: what the event thread does with it (copies it in);
 *   - apply_frame of an unpriced one: both, on the event thread (a snapshot nothing priced).
 *
 * Above 256 options pricing splits the IV and Greeks across the shared thread pool (a fork/join
 * per snapshot; it used to spawn a thread per core per call, the thread create/join dwarfing the
 * work).
 */

#include "bench_util.hpp"
#include "../../model/object.hpp"
#include "../../model/portfolio.hpp"

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
    model::PortfolioData portfolio{"BENCH"};
    model::PortfolioSnapshot snap;
    std::vector<model::OptionData> storage; // stable backing for option_apply_order_ ptrs

    explicit Fixture(size_t n_options) {
        storage.resize(n_options);
        portfolio.underlying = std::make_unique<model::UnderlyingData>();

        const auto now = std::chrono::system_clock::now();
        const auto expiry = now + std::chrono::hours(24 * 7); // 7 DTE

        portfolio.option_apply_order_.reserve(n_options);
        for (size_t i = 0; i < n_options; ++i) {
            auto& opt = storage[i];
            // Strikes spread around a 5000 spot (SPX-like), alternating call/put.
            opt.strike = 4000.0 + static_cast<double>(i % 500) * 4.0;
            opt.type = (i % 2 == 0) ? model::OptionType::CALL : model::OptionType::PUT;
            opt.expiry = expiry;
            opt.size = 100.0;
            portfolio.option_apply_order_.push_back(&opt);
        }
        portfolio.lay_out_pricing();

        snap.portfolio_name = "BENCH";
        snap.datetime = now;
        snap.underlying_bid = 4998.0;
        snap.underlying_ask = 5002.0;
        snap.underlying_mark = 5000.0;
        auto fill = [n_options](std::vector<double>& v, double base) {
            v.resize(n_options);
            for (size_t i = 0; i < n_options; ++i) v[i] = base + static_cast<double>(i % 50) * 0.1;
        };
        fill(snap.bid, 10.0);
        fill(snap.ask, 10.4);
        fill(snap.mark, 10.2);
        // delta/gamma/... in the snapshot are inputs the engine recomputes; size them to n.
        snap.delta.resize(n_options);
        snap.gamma.resize(n_options);
        snap.theta.resize(n_options);
        snap.vega.resize(n_options);
        snap.iv.resize(n_options);
    }
};

void bench_snapshot(size_t n_options) {
    Fixture fx(n_options);
    // µs-scale → PER_OP timing gives a real per-op tail distribution.
    model::PortfolioSnapshot priced = fx.snap;
    Histogram pricing = bench::measure_per_op(
        [&] {
            priced.priced = false;
            fx.portfolio.price_snapshot(priced);
            keep(priced.iv[0]);
        },
        /*iters=*/5000, /*warmup=*/200);
    Histogram apply_priced = bench::measure_per_op(
        [&] {
            fx.portfolio.apply_frame(priced);
            keep(fx.portfolio.option_apply_order_[0]->mid_iv);
        },
        /*iters=*/5000, /*warmup=*/200);
    Histogram apply_unpriced = bench::measure_per_op(
        [&] {
            fx.portfolio.apply_frame(fx.snap);
            keep(fx.portfolio.option_apply_order_[0]->mid_iv);
        },
        /*iters=*/5000, /*warmup=*/200);
    const std::string n = std::to_string(n_options);
    bench::print_row(("price_snapshot " + n).c_str(), pricing);
    bench::print_row(("apply_frame priced " + n).c_str(), apply_priced);
    bench::print_row(("apply_frame unpriced " + n).c_str(), apply_unpriced);
}

} // namespace

int main() {
    bench::print_env();
    std::printf("NOTE: above 256 options pricing forks and joins on the shared thread pool.\n");

    bench::print_header("a snapshot's cost by options in it: pricing, applying (ns)");
    for (const size_t n : {64, 256, 1000, 4000}) { // 4000: a full SPXW-ish chain
        bench_snapshot(n);
    }

    return 0;
}
