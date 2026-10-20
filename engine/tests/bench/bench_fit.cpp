/**
 * What fitting a chain's smile costs (pricing::fit_smiles on the chain alone, as a backtest day's
 * chain is priced): every minute of every chain of the given days, and the chains with at most
 * max_dte days left on their own; the fits' points, error and butterfly arbitrage.
 *
 * Usage: bench_fit <day.parquet>... [--max-dte N]
 */

#include "tests/bench/bench_util.hpp"

#include "adapters/market_data/historical_data.hpp"
#include "model/portfolio.hpp"
#include "pricing/pricing.hpp"
#include "quant/engine_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Rows {
    bench::Histogram svi;      // a chain at max_dte or nearer
    bench::Histogram svi_all;  // any chain
    std::vector<double> errors;
    std::vector<double> errors_near; // at max_dte or nearer
    long fits = 0;
    long unfitted = 0;
    long arbitrage = 0;
    long points = 0;
    double day_cpu_ns = 0.0; // every chain's every minute, one after another
    int days = 0;
};

/** Chain c of the day alone, at minute i, as `full` holds it. */
void chain_alone(const model::PortfolioSnapshot& full, std::size_t c,
                 model::PortfolioSnapshot& alone) {
    const model::Listing::Chain& chain = full.listing->chains()[c];
    alone.datetime = full.datetime;
    alone.underlying_bid = full.underlying_bid;
    alone.underlying_ask = full.underlying_ask;
    alone.underlying_mark = full.underlying_mark;
    for (const auto column : {&model::Frame::bid, &model::Frame::ask, &model::Frame::iv}) {
        std::ranges::copy((full.*column).subspan(chain.first, chain.count),
                          alone.values(column).begin());
    }
    alone.values(&model::Frame::forward)[0] = full.forward[c];
    alone.values(&model::Frame::expiry_years)[0] = full.expiry_years[chain.expiry_index];
    alone.values(&model::Frame::expiry_rate)[0] = full.expiry_rate[chain.expiry_index];
    std::ranges::fill(alone.values(&model::Frame::smile), 0.0);
    alone.priced = true;
}

} // namespace

int main(int argc, char** argv) {
    bench::print_env();
    std::vector<std::string> files;
    int max_dte = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--max-dte" && i + 1 < argc) {
            max_dte = std::stoi(argv[++i]);
        } else {
            files.push_back(arg);
        }
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: %s <day.parquet>... [--max-dte N]\n", argv[0]);
        return 2;
    }
    Rows rows;
    for (const std::string& path : files) {
        adapters::HistoricalData day;
        day.load(path);
        if (!day.has_data()) {
            std::fprintf(stderr, "cannot load %s: %s\n", path.c_str(), day.load_error().c_str());
            return 1;
        }
        day.load_all_chains();
        ++rows.days;
        model::PortfolioSnapshot full(day.listing(), day.portfolio_name());
        const auto chains = day.listing()->chains();
        double day_ns = 0.0;
        for (std::size_t c = 0; c < chains.size(); ++c) {
            const int dte = static_cast<int>(
                std::chrono::duration_cast<std::chrono::hours>(chains[c].expiry - day.day())
                    .count() /
                24);
            std::vector<model::Contract> contracts(
                day.listing()->contracts().begin() + chains[c].first,
                day.listing()->contracts().begin() + chains[c].first + chains[c].count);
            const auto listing = std::make_shared<const model::Listing>(std::move(contracts));
            model::PortfolioSnapshot alone(listing, day.portfolio_name());
            for (std::size_t i = 0; i < day.minutes(); ++i) {
                day.fill(i, full);
                chain_alone(full, c, alone);
                const std::uint64_t t0 = bench::clock_ticks();
                pricing::fit_smiles(alone);
                const double ns = bench::ticks_to_ns(bench::clock_ticks() - t0);
                day_ns += ns;
                rows.svi_all.record_ns(ns);
                if (dte <= max_dte) {
                    rows.svi.record_ns(ns);
                }
                const std::span<const double> v = alone.smile;
                if (v[model::kSmilePoints] > 0.0) {
                    ++rows.fits;
                    rows.points += static_cast<long>(v[model::kSmilePoints]);
                    rows.errors.push_back(v[model::kSmileError]);
                    if (dte <= max_dte) {
                        rows.errors_near.push_back(v[model::kSmileError]);
                    }
                    rows.arbitrage += v[model::kSmileButterflyFree] == 0.0 ? 1 : 0;
                } else {
                    ++rows.unfitted;
                }
            }
        }
        rows.day_cpu_ns += day_ns;
        std::printf("%s: %zu chains, %zu minutes, fitting them all %.0f ms\n", path.c_str(),
                    chains.size(), day.minutes(), day_ns / 1e6);
    }
    bench::print_header("smile fit (one chain, a minute)");
    bench::print_row(("svi, dte <= " + std::to_string(max_dte)).c_str(), rows.svi);
    bench::print_row("svi, any chain", rows.svi_all);
    const auto print_errors = [](const char* which, std::vector<double>& e) {
        std::ranges::sort(e);
        const auto at = [&](double q) {
            const double last = static_cast<double>(e.size() - 1);
            return e.empty() ? 0.0 : e[static_cast<std::size_t>(q * last)];
        };
        std::printf("error %s (rms residual over half spread): p50 %.3f  p90 %.3f  p99 %.3f\n",
                    which, at(0.5), at(0.9), at(0.99));
    };
    std::printf("\nfits %ld (not fitted %ld), %.1f points each; butterfly arbitrage left in %ld\n",
                rows.fits, rows.unfitted,
                rows.fits > 0 ? static_cast<double>(rows.points) / rows.fits : 0.0,
                rows.arbitrage);
    print_errors(("dte <= " + std::to_string(max_dte)).c_str(), rows.errors_near);
    print_errors("any chain", rows.errors);
    std::printf("every chain, every minute: %.0f ms a day\n", rows.day_cpu_ns / 1e6 / rows.days);
    return 0;
}
