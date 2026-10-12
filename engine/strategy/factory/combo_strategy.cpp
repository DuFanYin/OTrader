#include "combo_strategy.hpp"
#include "strategy_registry.hpp"

#include "../../model/utility.hpp"
#include "../../quant/kernel.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <set>

namespace strategy {

namespace {

// name, default, min, max, integer
constexpr std::array kParameters{
    StrategyParam{"entry_minute", 30, 1, 389, true}, // 10:00
    StrategyParam{"exit_minute", 380, 2, 390, true}, // 15:50
    StrategyParam{"quantity", 1, 1, 100, true},      // combo units
    StrategyParam{"take_profit", 0, 0, 100, false},  // x the premium; 0: none
    StrategyParam{"stop_loss", 0, 0, 100, false},    // x the premium; 0: none
    StrategyParam{"leg1_ratio", 1, -10, 10, true},   // contracts per unit: bought +, sold -
    StrategyParam{"leg1_right", 0, 0, 1, true},      // 0 call, 1 put
    StrategyParam{"leg1_strike_pct", 0, -50, 50, false}, // percent from the forward
    StrategyParam{"leg1_dte", 0, 0, 365, true},      // the first expiry at least this many days out
    StrategyParam{"leg2_ratio", 0, -10, 10, true},
    StrategyParam{"leg2_right", 0, 0, 1, true},
    StrategyParam{"leg2_strike_pct", 0, -50, 50, false},
    StrategyParam{"leg2_dte", 0, 0, 365, true},
    StrategyParam{"leg3_ratio", 0, -10, 10, true},
    StrategyParam{"leg3_right", 0, 0, 1, true},
    StrategyParam{"leg3_strike_pct", 0, -50, 50, false},
    StrategyParam{"leg3_dte", 0, 0, 365, true},
    StrategyParam{"leg4_ratio", 0, -10, 10, true},
    StrategyParam{"leg4_right", 0, 0, 1, true},
    StrategyParam{"leg4_strike_pct", 0, -50, 50, false},
    StrategyParam{"leg4_dte", 0, 0, 365, true},
    StrategyParam{"hedge", 0, 0, 1, true},           // 1: the HedgeEngine keeps delta near 0
    StrategyParam{"hedge_timer", 1, 1, 390, true},   // hedge every this many bars
    StrategyParam{"delta_range", 20, 0, 1000, true}, // shares either side of zero left alone
    StrategyParam{"model", 0, 0, 1, true},           // 1: enter only on a Heston edge
    StrategyParam{"min_edge", 0, -1'000'000, 1'000'000, false}, // USD a combo unit
};

constexpr int kHestonStrikes = 40;    // the strikes nearest the forward it is fitted to
constexpr int kHestonIterations = 300;
constexpr int kNoExpiryLimit = 3650;

auto two_sided(const model::OptionData* option) -> bool {
    return option != nullptr && option->bid_price > 0.0 && option->ask_price > 0.0;
}

auto mid(const model::OptionData& option) -> double {
    return 0.5 * (option.bid_price + option.ask_price);
}

auto size_of(const model::OptionData& option) -> double {
    return option.size > 0.0 ? option.size : 100.0;
}

} // namespace

ComboStrategy::ComboStrategy(StrategyContext* context, const std::string& strategy_name,
                             const std::string& portfolio_name,
                             const std::unordered_map<std::string, double>& setting)
    : OptionStrategyTemplate(context, strategy_name, portfolio_name, setting),
      entry_minute_(param<int>("entry_minute")), exit_minute_(param<int>("exit_minute")),
      quantity_(param<int>("quantity")), take_profit_(param("take_profit")),
      stop_loss_(param("stop_loss")), hedge_(param<int>("hedge") != 0),
      hedge_timer_(param<int>("hedge_timer")), delta_range_(param<int>("delta_range")),
      model_(param<int>("model")), min_edge_(param("min_edge")) {
    for (int i = 0; i < kMaxLegs; ++i) {
        const std::string leg = std::format("leg{}_", i + 1);
        LegRule& rule = rules_[static_cast<std::size_t>(i)];
        rule.ratio = param<int>(leg + "ratio");
        rule.right = param<int>(leg + "right") == 0 ? model::OptionType::CALL
                                                    : model::OptionType::PUT;
        rule.strike_pct = param(leg + "strike_pct");
        rule.dte = param<int>(leg + "dte");
    }
}

auto ComboStrategy::parameters() -> std::span<const StrategyParam> { return kParameters; }

void ComboStrategy::on_init_logic() {
    std::string legs;
    for (const LegRule& rule : rules_) {
        if (rule.ratio != 0) {
            legs += std::format("{}{:+d} {} {:+.2f}% {}d", legs.empty() ? "" : ", ", rule.ratio,
                                rule.right == model::OptionType::CALL ? "C" : "P",
                                rule.strike_pct, rule.dte);
        }
    }
    if (legs.empty()) {
        set_error("ComboStrategy: no leg has a ratio");
        return;
    }
    if (hedge_) {
        register_hedging(hedge_timer_, 0, delta_range_);
    }
    write_log(std::format("Combo {} x{}, minute {} to {}{}{}", legs, quantity_, entry_minute_,
                          exit_minute_,
                          hedge_ ? std::format(", hedged within {} shares", delta_range_) : "",
                          model_ == 1 ? std::format(", Heston edge >= {:.2f}", min_edge_) : ""));
}

void ComboStrategy::on_stop_logic() { close_all_strategy_positions(); }

void ComboStrategy::start_day(std::chrono::year_month_day day) {
    day_ = day;
    entered_ = false;
    done_ = false;
    premium_ = 0.0;
    std::set<std::string> subscribe;
    for (int i = 0; i < kMaxLegs; ++i) {
        const LegRule& rule = rules_[static_cast<std::size_t>(i)];
        std::string& chain = chains_[static_cast<std::size_t>(i)];
        chain.clear();
        if (rule.ratio == 0) {
            continue;
        }
        const std::vector<std::string> found =
            portfolio()->get_chain_by_expiry(rule.dte, kNoExpiryLimit);
        if (found.empty()) {
            write_log(std::format("{}: no chain {} days out or more: no trade", day, rule.dte),
                      utilities::WARNING);
            done_ = true;
            return;
        }
        chain = found.front();
        subscribe.insert(chain);
    }
    subscribe_chains(std::vector<std::string>(subscribe.begin(), subscribe.end()));
}

auto ComboStrategy::holds_options() const -> bool {
    if (holding() == nullptr) {
        return false;
    }
    bool held = false;
    holding()->for_each_open_option([&held](const model::BasePosition&) { held = true; });
    return held;
}

auto ComboStrategy::pick_options() const -> std::vector<const model::OptionData*> {
    std::vector<const model::OptionData*> picked;
    for (int i = 0; i < kMaxLegs; ++i) {
        const LegRule& rule = rules_[static_cast<std::size_t>(i)];
        if (rule.ratio == 0) {
            continue;
        }
        const model::ChainData* chain = get_chain(chains_[static_cast<std::size_t>(i)]);
        if (chain == nullptr || chain->forward <= 0.0 || chain->strikes.empty()) {
            return {};
        }
        const double target = chain->forward * (1.0 + rule.strike_pct / 100.0);
        const auto nearest = std::ranges::min_element(chain->strikes, {}, [target](double k) {
            return std::abs(k - target);
        });
        const auto& side = rule.right == model::OptionType::CALL ? chain->calls : chain->puts;
        const auto it = side.find(*nearest);
        if (it == side.end() || !two_sided(it->second)) {
            return {};
        }
        if (std::ranges::find(picked, it->second) != picked.end()) {
            return {}; // two legs on one contract: not a combo the venue takes
        }
        picked.push_back(it->second);
    }
    return picked;
}

auto ComboStrategy::heston_edge(std::span<const model::OptionData* const> options) const
    -> double {
    const double rate = portfolio()->risk_free_rate_;
    double value = 0.0;
    double cost = 0.0;
    std::unordered_map<std::string, quant::HestonParams> fitted; // by chain
    std::size_t leg = 0;
    for (int i = 0; i < kMaxLegs; ++i) {
        const LegRule& rule = rules_[static_cast<std::size_t>(i)];
        if (rule.ratio == 0) {
            continue;
        }
        const model::OptionData& option = *options[leg++];
        const std::string& chain_symbol = chains_[static_cast<std::size_t>(i)];
        const model::ChainData* chain = get_chain(chain_symbol);
        const double years = model::years_to_expiry(now(), option.expiry);
        if (chain == nullptr || chain->forward <= 0.0 || years <= 0.0) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        const double forward = chain->forward;
        const double discount = std::exp(-rate * years);
        if (!fitted.contains(chain_symbol)) {
            // Out-of-the-money mids as call prices (a put's by parity), nearest the forward
            std::vector<std::pair<double, double>> quotes; // strike, call price
            for (const double k : chain->strikes) {
                const auto call = chain->calls.find(k);
                const auto put = chain->puts.find(k);
                if (k >= forward && call != chain->calls.end() && two_sided(call->second)) {
                    quotes.emplace_back(k, mid(*call->second));
                } else if (k < forward && put != chain->puts.end() && two_sided(put->second)) {
                    quotes.emplace_back(k, mid(*put->second) + discount * (forward - k));
                }
            }
            std::ranges::sort(quotes, {}, [forward](const auto& q) {
                return std::abs(q.first - forward);
            });
            quotes.resize(std::min<std::size_t>(quotes.size(), kHestonStrikes));
            if (quotes.size() < 5) {
                return std::numeric_limits<double>::quiet_NaN();
            }
            std::vector<double> strikes;
            std::vector<double> prices;
            for (const auto& [k, price] : quotes) {
                strikes.push_back(k);
                prices.push_back(price);
            }
            const std::vector<double> maturities(strikes.size(), years);
            fitted[chain_symbol] = quant::heston_calibrate(forward * discount, rate, strikes,
                                                           maturities, prices, {},
                                                           kHestonIterations)
                                       .params;
        }
        const quant::HestonParams& h = fitted[chain_symbol];
        const double call = quant::heston_call_price(forward * discount, option.strike, rate,
                                                     years, h.v0, h.kappa, h.theta, h.xi, h.rho);
        const double price = option.type == model::OptionType::CALL
                                 ? call
                                 : call - discount * (forward - option.strike);
        value += rule.ratio * price * size_of(option);
        cost += rule.ratio * (rule.ratio > 0 ? option.ask_price : option.bid_price) *
                size_of(option);
    }
    return value - cost;
}

void ComboStrategy::enter(int minute) {
    const std::vector<const model::OptionData*> options = pick_options();
    if (options.empty()) {
        return; // a leg not quoted both ways yet: the next bar
    }
    entered_ = true;
    if (model_ == 1) {
        const double edge = heston_edge(options);
        if (std::isnan(edge) || edge < min_edge_) {
            write_log(std::format("Minute {}: Heston edge {:.2f} USD a unit, under {:.2f}: no "
                                  "trade",
                                  minute, edge, min_edge_));
            done_ = true;
            return;
        }
        write_log(std::format("Minute {}: Heston edge {:.2f} USD a unit", minute, edge));
    }
    std::vector<model::Leg> legs;
    std::string names;
    double premium = 0.0;
    std::size_t leg = 0;
    try {
        for (const LegRule& rule : rules_) {
            if (rule.ratio == 0) {
                continue;
            }
            const model::OptionData& option = *options[leg++];
            legs.push_back(combo_leg(option,
                                     rule.ratio > 0 ? model::Direction::LONG
                                                    : model::Direction::SHORT,
                                     std::abs(rule.ratio)));
            premium += rule.ratio * mid(option) * size_of(option) * quantity_;
            names += std::format("{}{:+d} {}", names.empty() ? "" : ", ", rule.ratio,
                                 option.symbol);
        }
    } catch (const std::exception& e) {
        write_log(std::string("Combo not sent: ") + e.what(), utilities::ERROR);
        done_ = true;
        return;
    }
    if (custom_combo_order(legs, 0.0, quantity_, model::OrderType::MARKET).empty()) {
        done_ = true; // refused (the risk check says why): no second try today
        return;
    }
    premium_ = std::abs(premium);
    pnl_at_entry_ = holding() != nullptr ? holding()->summary.pnl : 0.0;
    write_log(std::format("Minute {}: opens {} x{} for about {:.2f} USD", minute, names,
                          quantity_, premium));
}

void ComboStrategy::on_timer_logic() {
    const model::DateTime t = now();
    const std::chrono::year_month_day today = model::new_york_date(t);
    if (today != day_) {
        start_day(today);
    }
    const int minute = model::minutes_since_open(t);
    if (minute < 1 || minute > 390 || has_active_order()) {
        return;
    }
    if (entered_) {
        if (done_ || !holds_options()) {
            return;
        }
        const double pnl = holding()->summary.pnl - pnl_at_entry_;
        const bool profit = take_profit_ > 0.0 && premium_ > 0.0 && pnl >= take_profit_ * premium_;
        const bool loss = stop_loss_ > 0.0 && premium_ > 0.0 && pnl <= -stop_loss_ * premium_;
        if (minute >= exit_minute_ || profit || loss) {
            write_log(std::format("Minute {}: closes ({}), P&L {:.2f}", minute,
                                  profit ? "take profit" : loss ? "stop loss" : "exit minute", pnl));
            close_all_strategy_positions();
            done_ = true;
        }
        return;
    }
    if (!done_ && minute >= entry_minute_ && minute < exit_minute_) {
        enter(minute);
    }
}

REGISTER_STRATEGY(ComboStrategy);

} // namespace strategy
