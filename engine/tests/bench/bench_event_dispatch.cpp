/**
 * What a snapshot costs, by chain size, on a SYNTHETIC fixture sized to a realistic SPXW chain
 * (controlled + repeatable):
 *   - price_snapshot: forwards, IV and Greeks for the whole chain, where live pricing runs (the
 *     market data thread; a backtest before its run);
 *   - apply of a priced snapshot: what the event thread does with it (takes over its values);
 *   - apply of a priced frame read in place (a backtest's bar);
 *   - apply of an unpriced frame: copied and priced on the event thread.
 *
 * Above 256 options pricing splits the IV and Greeks across the shared thread pool.
 */

#include "tests/bench/bench_util.hpp"
#include "model/object.hpp"
#include "model/portfolio.hpp"
#include "model/symbol.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

using bench::Histogram;
using bench::keep;

namespace {

// A portfolio listing `n_options` synthetic options (1000 a weekly expiry: 500 strikes around a
// 5000 spot, a call and a put each) and a snapshot of them.
struct Fixture {
    model::Portfolio portfolio{"BENCH"};
    model::PortfolioSnapshot snap;

    explicit Fixture(size_t n_options) {
        using namespace std::chrono;
        const auto now = system_clock::now();
        std::vector<model::Contract> contracts;
        for (size_t i = 0; i < n_options; ++i) {
            const year_month_day expiry{floor<days>(now) + days(7 * (1 + i / 1000))};
            model::Contract c;
            c.symbol = model::option_symbol(
                {.root = "BENCH",
                 .expiry = expiry,
                 .type = i % 2 == 0 ? model::OptionType::CALL : model::OptionType::PUT,
                 .strike = 4000.0 + (static_cast<double>((i / 2) % 500) * 4.0)});
            c.multiplier = 100.0;
            contracts.push_back(std::move(c));
        }
        portfolio.list(std::make_shared<const model::Listing>(contracts));

        snap = portfolio.snapshot();
        snap.datetime = now;
        snap.underlying_bid = 4998.0;
        snap.underlying_ask = 5002.0;
        snap.underlying_mark = 5000.0;
        const auto fill = [](std::span<double> v, double base) {
            for (size_t i = 0; i < v.size(); ++i) {
                v[i] = base + static_cast<double>(i % 50) * 0.1;
            }
        };
        fill(snap.values(&model::Frame::bid), 10.0);
        fill(snap.values(&model::Frame::ask), 10.4);
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
    const model::Option first(fx.portfolio, 0);
    model::PortfolioSnapshot taken = priced;
    Histogram apply_priced = bench::measure_per_op(
        [&] {
            fx.portfolio.apply(taken); // the two swap values each time
            keep(first.iv());
        },
        /*iters=*/5000, /*warmup=*/200);
    const auto stored = std::make_shared<model::PortfolioSnapshot>(priced);
    Histogram apply_in_place = bench::measure_per_op(
        [&] {
            fx.portfolio.apply(*stored, stored);
            keep(first.iv());
        },
        /*iters=*/5000, /*warmup=*/200);
    Histogram apply_unpriced = bench::measure_per_op(
        [&] {
            fx.portfolio.apply(static_cast<const model::Frame&>(fx.snap));
            keep(first.iv());
        },
        /*iters=*/5000, /*warmup=*/200);
    const std::string n = std::to_string(n_options);
    bench::print_row(("price_snapshot " + n).c_str(), pricing);
    bench::print_row(("apply snapshot " + n).c_str(), apply_priced);
    bench::print_row(("apply frame in place " + n).c_str(), apply_in_place);
    bench::print_row(("apply unpriced " + n).c_str(), apply_unpriced);
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
