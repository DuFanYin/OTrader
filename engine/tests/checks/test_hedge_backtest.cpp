/**
 * Delta hedging end to end, on synthetic days priced by a known model
 * (POST /api/data/synthetic, or scripts/data/synthetic_to_parquet.py, --tick 0 --spread 0): DeltaHedgedStraddleStrategy
 * holds an ATM straddle and the HedgeEngine trades the underlying. Every bar it checks
 *   - the hedge: each underlying trade is the whole number of shares that brings the last bar's
 *     delta back to zero, when that delta was outside the range, and there is none otherwise;
 *   - the delta: the platform's per-option delta against the delta of the model that priced
 *     the quotes (Black on spot·e^(rt), discounted at r: Black-Scholes at --rate);
 *   - the P&L: the day's P&L against its attribution, the sum over bars of
 *     Δ·dS + ½Γ·dS² − ½Γ·σ²S²·dt (the delta held, gamma, and the theta the quotes' vol implies).
 *
 * Usage: test_hedge_backtest --iv 0.15 [--side -1] [--size 10] [--range 20] [--hedge 1]
 *                            [--rate 0.05] [--engine-rate R] day.parquet...
 * Exits 1 when a hedge trade is not the one the delta called for. A week of options richer than
 * the moves (a sold straddle, hedged, should earn every day):
 *   uv run --project app/server python scripts/data/synthetic_to_parquet.py --start 2025-03-10 --end 2025-03-14 --out /tmp/rich
 *       --tick 0 --spread 0 --realized-vol 0.10 --implied-vol 0.25
 *   test_hedge_backtest --iv 0.25 (the day files in /tmp/rich/SPY/SPY-2025-03)
 * --rate is the rate the data was priced at (synthetic_to_parquet.py --rate); --engine-rate the
 * engine's --risk-free-rate (default the same): its forward comes from the quotes, so a wrong
 * rate barely moves its delta.
 */

#include "quant/kernel.h"
#include "core/engine_backtest.hpp"
#include "log_level.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr double kYearSeconds = 365.25 * 24 * 3600; // model::years_to_expiry's year

struct Options {
    double iv = 0.0;
    double side = -1;
    double size = 10;
    double range = 20;
    double hedge = 1;
    double rate = 0.05;
    double engine_rate = -1; // -1: rate
    std::vector<std::string> files;
};

struct Day {
    std::string file;
    double open = 0.0;
    double close = 0.0;
    int hedge_trades = 0;
    double shares_traded = 0.0;
    int wrong_hedges = 0;
    double max_net_delta = 0.0;    // the strategy's delta, options and shares, before each hedge
    double max_option_delta = 0.0; // the straddle's alone: the delta left unhedged
    double max_delta_error = 0.0;  // per-option delta, platform vs the quotes' model
    double pnl = 0.0;
    double attribution = 0.0;
};

/** The bar's state the next bar is checked against. */
struct Bar {
    double spot = 0.0;
    double delta = 0.0; // the strategy's, before this bar's hedge
    double gamma = 0.0;
    int shares = 0;
    model::DateTime at{};
};

auto parse(int argc, char* argv[]) -> std::optional<Options> {
    Options o;
    const std::unordered_map<std::string, double*> flags{
        {"--iv", &o.iv},
        {"--side", &o.side},
        {"--size", &o.size},
        {"--range", &o.range},
        {"--hedge", &o.hedge},
        {"--rate", &o.rate},
        {"--engine-rate", &o.engine_rate},
    };
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (const auto it = flags.find(arg); it != flags.end() && i + 1 < argc) {
            *it->second = std::stod(argv[++i]);
        } else {
            o.files.push_back(arg);
        }
    }
    if (o.iv <= 0.0 || o.files.empty()) {
        return std::nullopt;
    }
    if (o.engine_rate < 0.0) {
        o.engine_rate = o.rate;
    }
    return o;
}

/** What hedging every bar should trade after a bar whose delta was `delta`: the whole shares back
 * to zero when it is outside the range (the HedgeEngine's delta_target 0). */
auto expected_hedge(double delta, double range) -> int {
    if (std::abs(delta) <= range) {
        return 0;
    }
    const double shares = std::round(-delta);
    return std::abs(shares) >= 1.0 ? static_cast<int>(shares) : 0;
}

auto run_day(const Options& o, const std::string& file) -> Day {
    Day day;
    day.file = file;
    core::backtest::MainEngine engine;
    engine.configure_execution(0.0, 0.0); // no fees or slippage: the P&L is the hedging's
    engine.set_log_level(utilities::DISABLED);
    engine.load_backtest_data(file);
    if (engine.get_data_engine() == nullptr || !engine.get_data_engine()->has_data()) {
        std::println(stderr, "{}: no data", file);
        std::exit(2);
    }
    engine.add_strategy("DeltaHedgedStraddleStrategy", engine.get_data_engine()->portfolio_name(),
                        {{"position_size", o.size},
                         {"side", o.side},
                         {"hedge", o.hedge},
                         {"hedge_timer", 1},
                         {"delta_range", o.range},
                         {"min_dte", 1},
                         {"max_dte", 7}});
    engine.get_data_engine()->set_risk_free_rate(o.engine_rate);

    core::backtest::MainEngine* main = &engine;
    std::optional<Bar> last;
    engine.register_timestep_callback([&](int /*timestep*/, adapters::Timestamp /*ts*/) {
        model::PortfolioData* portfolio = main->get_portfolio(main->get_data_engine()->portfolio_name());
        const model::StrategyHolding* holding =
            main->option_strategy_engine()->get_strategy_holding();
        if (portfolio == nullptr || holding == nullptr || !portfolio->underlying) {
            return;
        }
        const model::UnderlyingData& u = *portfolio->underlying;
        const Bar bar{.spot = 0.5 * (u.bid_price + u.ask_price),
                      .delta = holding->summary.delta,
                      .gamma = holding->summary.gamma,
                      .shares = holding->underlyingPosition.quantity,
                      .at = portfolio->datetime()};
        if (day.open == 0.0) {
            day.open = bar.spot;
        }
        day.close = bar.spot;

        // The straddle's legs: their delta, and each option's against the model's.
        double option_delta = 0.0;
        for (const auto& [_, position] : holding->optionPositions) {
            for (const auto& leg : position.legs) {
                const auto it = portfolio->options.find(leg.symbol);
                if (leg.quantity == 0 || it == portfolio->options.end()) {
                    continue;
                }
                const model::OptionData& opt = it->second;
                option_delta += leg.quantity * opt.delta;
                const double t = model::years_to_expiry(bar.at, opt.expiry);
                const bool call = opt.type == model::OptionType::CALL;
                const double k = opt.strike;
                const double platform = opt.delta / opt.size;
                // Black-Scholes at --rate: Black on spot·e^(rt), discounted at r.
                const double forward = bar.spot * std::exp(o.rate * t);
                const double model =
                    quant::black_greeks(call, bar.spot, forward, k, t, o.rate, o.iv).delta;
                day.max_delta_error = std::max(day.max_delta_error, std::abs(platform - model));
            }
        }
        day.max_option_delta = std::max(day.max_option_delta, std::abs(option_delta));
        day.max_net_delta = std::max(day.max_net_delta, std::abs(bar.delta));

        if (last) {
            const int traded = bar.shares - last->shares;
            if (traded != 0) {
                ++day.hedge_trades;
                day.shares_traded += std::abs(traded);
            }
            const int want = o.hedge != 0.0 ? expected_hedge(last->delta, o.range) : 0;
            if (traded != want) {
                if (day.wrong_hedges++ < 5) {
                    const std::chrono::zoned_time ny{
                        "America/New_York", std::chrono::floor<std::chrono::minutes>(bar.at)};
                    std::println("  {:%H:%M} ET: delta {:.2f} called for {} shares, traded {}", ny,
                                 last->delta, want, traded);
                }
            }
            // The bar's P&L as its Greeks explain it: the delta held over it (the last delta,
            // and the hedge traded at the bar's start), gamma, and theta at the quotes' vol.
            const double ds = bar.spot - last->spot;
            const double dt =
                std::chrono::duration<double>(bar.at - last->at).count() / kYearSeconds;
            day.attribution += (last->delta + traded) * ds + 0.5 * last->gamma * ds * ds -
                               0.5 * last->gamma * o.iv * o.iv * last->spot * last->spot * dt;
        }
        day.pnl = holding->summary.pnl;
        last = bar;
    });
    engine.run();
    return day;
}

} // namespace

int main(int argc, char* argv[]) {
    const auto options = parse(argc, argv);
    if (!options) {
        std::println(stderr, "usage: test_hedge_backtest --iv VOL [--side -1|1] [--size N] "
                             "[--range SHARES] [--hedge 0|1] [--rate R] [--engine-rate R] "
                             "day.parquet...");
        return 2;
    }
    const Options& o = *options;
    std::println("{} {} straddles, hedge {}, quotes' vol {:.0f}%, rate {} (engine's {})",
                 o.side > 0 ? "long" : "short", o.size,
                 o.hedge != 0.0 ? std::format("within {} shares", o.range) : std::string("off"),
                 o.iv * 100, o.rate, o.engine_rate);
    std::println("{:<10} {:>7} {:>6} {:>7} {:>8} {:>8} {:>9} {:>9} {:>9} {:>8}", "day", "move%",
                 "hedges", "shares", "maxNetΔ", "maxOptΔ", "P&L", "attrib", "attr-P&L", "δ error");
    int wrong = 0;
    double total = 0.0;
    for (const std::string& file : o.files) {
        const Day d = run_day(o, file);
        const std::string name = file.substr(file.find_last_of('/') + 1, 8);
        std::println("{:<10} {:>7.2f} {:>6} {:>7.0f} {:>8.1f} {:>8.1f} {:>9.2f} {:>9.2f} {:>9.2f} "
                     "{:>8.4f}",
                     name, 100 * (d.close / d.open - 1), d.hedge_trades, d.shares_traded,
                     d.max_net_delta, d.max_option_delta, d.pnl, d.attribution,
                     d.attribution - d.pnl, d.max_delta_error);
        wrong += d.wrong_hedges;
        total += d.pnl;
    }
    std::println("total P&L {:.2f}; hedge trades not the ones the delta called for: {}", total,
                 wrong);
    return wrong == 0 ? 0 : 1;
}
