#include "combo_strategy.hpp"
#include "strategy_registry.hpp"

#include "model/utility.hpp"
#include "quant/kernel.h"
#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <set>

namespace strategy {

namespace {

constexpr int kHestonStrikes = 40;    // the strikes nearest the forward it is fitted to
constexpr int kHestonIterations = 300;
constexpr int kNoExpiryLimit = 3650;

auto two_sided(const std::optional<model::Option>& option) -> bool {
    return option && option->bid() > 0.0 && option->ask() > 0.0;
}

/** Quoted on the side a leg trades: an ask to buy, a bid to sell (a far wing often has no bid). */
auto quoted_for(const std::optional<model::Option>& option, int ratio) -> bool {
    return option && (ratio > 0 ? option->ask() : option->bid()) > 0.0;
}

auto mid(const model::Option& option) -> double { return 0.5 * (option.bid() + option.ask()); }

auto multiplier_of(const model::Option& option) -> double {
    return option.multiplier() > 0.0 ? option.multiplier() : 100.0;
}

} // namespace

auto ComboStrategy::rules_of(const Setting& s) -> std::array<LegRule, kMaxLegs> {
    const auto rule = [](int ratio, int right, double strike_pct, int dte) {
        return LegRule{.ratio = ratio,
                       .right = right == 0 ? model::OptionType::CALL : model::OptionType::PUT,
                       .strike_pct = strike_pct,
                       .dte = dte};
    };
    return {rule(s.leg1_ratio, s.leg1_right, s.leg1_strike_pct, s.leg1_dte),
            rule(s.leg2_ratio, s.leg2_right, s.leg2_strike_pct, s.leg2_dte),
            rule(s.leg3_ratio, s.leg3_right, s.leg3_strike_pct, s.leg3_dte),
            rule(s.leg4_ratio, s.leg4_right, s.leg4_strike_pct, s.leg4_dte)};
}

void ComboStrategy::on_init() {
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
    const Setting& s = setting_;
    if (s.hedge != 0) {
        register_hedging(s.hedge_timer, 0, s.delta_range);
    }
    const std::string until =
        s.hold != 0 ? std::string("its expiry") : std::format("{}", s.exit_minute);
    const std::string hedged =
        s.hedge != 0 ? std::format(", hedged within {} shares", s.delta_range) : "";
    const std::string edge =
        s.model == 1 ? std::format(", Heston edge >= {:.2f}", s.min_edge) : "";
    const std::string closes = s.close_itm_only != 0 ? " (legs in the money)" : "";
    const std::string expiry = s.exact_dte != 0 ? ", expiries exactly dte out" : "";
    const std::string filters =
        std::format(", day before's realized vol {:g}..{:g}%, move {:g}..{:g}%", s.prev_rv_min,
                    s.prev_rv_max, s.move_min, s.move_max);
    write_log(std::format("Combo {} x{}, minute {} to {}{}{}{}{}{}", legs, s.quantity,
                          s.entry_minute, until, closes, hedged, edge, expiry, filters));
}

void ComboStrategy::on_stop() { close_all_strategy_positions(); }

void ComboStrategy::on_day() {
    // From the values by day, not from the minutes it saw: the same after a restart in the day.
    prev_rv_ = daily(portfolio()->name + "_minute_rv")
                   .value_or(std::numeric_limits<double>::quiet_NaN());
    last_minute_ = -1;
    first_price_ = last_price_ = 0.0;
    done_ = false;
    if (!holds_options()) { // a combo held from a day before stays the one entered
        entered_ = false;
        premium_ = 0.0;
        if (traded_today()) { // started again after today's combo was closed
            done_ = true;
            write_log("Opened and closed a combo today already: no second one");
        }
    } else if (!entered_) { // started again holding it: what it cost, from the holding
        entered_ = true;
        premium_ = 0.0;
        holding()->for_each_open_option(
            [this](const model::OptionPosition& p) { premium_ += p.cost_value; });
        premium_ = std::abs(premium_);
        pnl_at_entry_ = std::numeric_limits<double>::quiet_NaN(); // set at the first bar marked
        write_log(std::format("Holds a combo opened before it started: {:.2f} USD", premium_));
    }
    std::set<std::string> subscribe;
    for (int i = 0; i < kMaxLegs; ++i) {
        const LegRule& rule = rules_[static_cast<std::size_t>(i)];
        std::string& chain = chains_[static_cast<std::size_t>(i)];
        chain.clear();
        if (rule.ratio == 0) {
            continue;
        }
        const bool exact = setting_.exact_dte != 0;
        const std::vector<std::string> found =
            portfolio()->chains_by_expiry(rule.dte, exact ? rule.dte : kNoExpiryLimit);
        if (found.empty()) {
            write_log(std::format("{}: no chain {} days out{}: no fill", day(), rule.dte,
                                  exact ? "" : " or more"),
                      exact ? utilities::INFO : utilities::WARNING);
            done_ = true;
            return;
        }
        chain = found.front();
        subscribe.insert(chain);
    }
    subscribe_chains(std::vector<std::string>(subscribe.begin(), subscribe.end()));
    sell_delivered_ = setting_.hedge == 0 && !holds_options() && holding() != nullptr &&
                      holding()->shares.quantity != 0;
}

auto ComboStrategy::pick_options() const -> std::vector<model::Option> {
    std::vector<model::Option> picked;
    for (int i = 0; i < kMaxLegs; ++i) {
        const LegRule& rule = rules_[static_cast<std::size_t>(i)];
        if (rule.ratio == 0) {
            continue;
        }
        const std::optional<model::Chain> chain = find_chain(chains_[static_cast<std::size_t>(i)]);
        if (!chain || chain->forward() <= 0.0 || chain->strikes().empty()) {
            return {};
        }
        const double target = chain->forward() * (1.0 + rule.strike_pct / 100.0);
        const auto nearest = std::ranges::min_element(chain->strikes(), {}, [target](double k) {
            return std::abs(k - target);
        });
        const std::optional<model::Option> option = chain->option(rule.right, *nearest);
        if (!quoted_for(option, rule.ratio)) {
            return {};
        }
        if (std::ranges::contains(picked, *option)) {
            return {}; // two legs on one contract: not a combo the venue takes
        }
        picked.push_back(*option);
    }
    return picked;
}

auto ComboStrategy::heston_edge(std::span<const model::Option> options) const -> double {
    double value = 0.0;
    double cost = 0.0;
    std::unordered_map<std::string, quant::HestonParams> fitted; // by chain
    std::size_t next = 0;
    for (int i = 0; i < kMaxLegs; ++i) {
        const LegRule& rule = rules_[static_cast<std::size_t>(i)];
        if (rule.ratio == 0) {
            continue;
        }
        const model::Option& option = options[next++];
        const std::string& chain_symbol = chains_[static_cast<std::size_t>(i)];
        const std::optional<model::Chain> chain = find_chain(chain_symbol);
        const double years = model::years_to_expiry(now(), option.expiry());
        if (!chain || chain->forward() <= 0.0 || years <= 0.0) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        const double forward = chain->forward();
        const double rate = chain->rate();
        const double discount = std::exp(-rate * years);
        if (!fitted.contains(chain_symbol)) {
            // OTM mids as call prices (puts by parity)
            std::vector<std::pair<double, double>> quotes; // strike, call price
            for (const double k : chain->strikes()) {
                const std::optional<model::Option> call = chain->call(k);
                const std::optional<model::Option> put = chain->put(k);
                if (k >= forward && two_sided(call)) {
                    quotes.emplace_back(k, mid(*call));
                } else if (k < forward && two_sided(put)) {
                    quotes.emplace_back(k, mid(*put) + discount * (forward - k));
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
        const double call = quant::heston_call_price(forward * discount, option.strike(), rate,
                                                     years, h.v0, h.kappa, h.theta, h.xi, h.rho);
        const double price = option.type() == model::OptionType::CALL
                                 ? call
                                 : call - discount * (forward - option.strike());
        value += rule.ratio * price * multiplier_of(option);
        cost += rule.ratio * (rule.ratio > 0 ? option.ask() : option.bid()) * multiplier_of(option);
    }
    return value - cost;
}

void ComboStrategy::enter(int minute) {
    const std::vector<model::Option> options = pick_options();
    if (options.empty()) {
        return; // a leg not quoted on its side yet: the next bar
    }
    entered_ = true;
    if (setting_.model == 1) {
        const double edge = heston_edge(options);
        if (std::isnan(edge) || edge < setting_.min_edge) {
            write_log(std::format("Minute {}: Heston edge {:.2f} USD a unit, under {:.2f}: no "
                                  "fill",
                                  minute, edge, setting_.min_edge));
            done_ = true;
            return;
        }
        write_log(std::format("Minute {}: Heston edge {:.2f} USD a unit", minute, edge));
    }
    std::vector<model::Leg> legs;
    std::string names;
    double premium = 0.0;
    std::size_t next = 0;
    try {
        for (const LegRule& rule : rules_) {
            if (rule.ratio == 0) {
                continue;
            }
            const model::Option& option = options[next++];
            legs.push_back(leg(option.symbol(),
                               rule.ratio > 0 ? model::Side::BUY : model::Side::SELL,
                               std::abs(rule.ratio)));
            premium += rule.ratio * mid(option) * multiplier_of(option) * setting_.quantity;
            names += std::format("{}{:+d} {}", names.empty() ? "" : ", ", rule.ratio,
                                 option.symbol());
        }
    } catch (const std::exception& e) {
        write_log(std::string("Combo not sent: ") + e.what(), utilities::ERROR);
        done_ = true;
        return;
    }
    if (send_order(legs, 0.0, setting_.quantity).empty()) {
        done_ = true; // refused (the risk check says why): no second try today
        return;
    }
    premium_ = std::abs(premium);
    pnl_at_entry_ = holding() != nullptr ? holding()->summary.pnl : 0.0;
    write_log(std::format("Minute {}: opens {} x{} for about {:.2f} USD", minute, names,
                          setting_.quantity, premium));
}

void ComboStrategy::track_underlying(int minute) {
    if (minute == last_minute_) {
        return; // live: a tick a second, the returns are a minute's
    }
    last_minute_ = minute;
    const std::optional<model::Underlying> underlying = portfolio()->underlying();
    const double price = underlying ? underlying->mid() : 0.0;
    if (price <= 0.0) {
        return;
    }
    if (first_price_ <= 0.0) {
        first_price_ = price;
    }
    last_price_ = price;
}

auto ComboStrategy::filters_allow(int minute) -> bool {
    const Setting& s = setting_;
    if (s.prev_rv_min > 0.0 || s.prev_rv_max < 100.0) {
        if (std::isnan(prev_rv_) || prev_rv_ < s.prev_rv_min || prev_rv_ > s.prev_rv_max) {
            write_log(std::format("Minute {}: the day before's realized vol {:.3f}% is not within "
                                  "{:g}..{:g}%: no combo today",
                                  minute, prev_rv_, s.prev_rv_min, s.prev_rv_max));
            done_ = true;
            return false;
        }
    }
    if (s.move_min > 0.0 || s.move_max < 100.0) {
        if (first_price_ <= 0.0) {
            return false; // no price yet: the next bar
        }
        const double move = std::abs(last_price_ / first_price_ - 1.0) * 100.0;
        if (move < s.move_min || move > s.move_max) {
            write_log(std::format("Minute {}: the move since the open {:.3f}% is not within "
                                  "{:g}..{:g}%: no combo today",
                                  minute, move, s.move_min, s.move_max));
            done_ = true;
            return false;
        }
    }
    return true;
}

void ComboStrategy::close_in_the_money(int minute) {
    const double price = last_price_;
    std::vector<model::OptionPosition> in_the_money;
    std::string names;
    for (const auto& [symbol, position] : holding()->options) {
        const std::optional<model::OptionId> id = model::parse_option_symbol(symbol);
        if (position.quantity == 0 || !id || price <= 0.0) {
            continue;
        }
        const bool call = id->type == model::OptionType::CALL;
        if (call ? price > id->strike : price < id->strike) {
            in_the_money.push_back(position);
            names += std::format("{}{}", names.empty() ? "" : ", ", symbol);
        }
    }
    write_log(std::format("Minute {}: underlying {:.2f}; closes the legs in the money ({}), the "
                          "rest expire",
                          minute, price, names.empty() ? "none" : names));
    if (!in_the_money.empty()) {
        close_options(in_the_money);
    }
}

auto ComboStrategy::resume_pnl_at_entry() -> bool {
    double unrealized = 0.0;
    bool valued = true;
    holding()->for_each_open_option([&](const model::OptionPosition& p) {
        valued = valued && p.mid_price > 0.0;
        unrealized += p.current_value() - p.cost_value;
    });
    if (valued) {
        pnl_at_entry_ = holding()->summary.pnl - unrealized;
    }
    return valued;
}

void ComboStrategy::on_timer() {
    const int minute = this->minute();
    if (minute < 1 || minute > 390) {
        return;
    }
    track_underlying(minute);
    if (has_active_order()) {
        return;
    }
    if (sell_delivered_) { // the day's first bar: the market is open
        sell_delivered_ = false;
        write_log(std::format("{}: sells the {} shares a settlement delivered", day(),
                              holding()->shares.quantity));
        close_all_strategy_positions();
        return;
    }
    if (entered_) {
        if (done_ || !holds_options()) {
            return;
        }
        if (std::isnan(pnl_at_entry_) && !resume_pnl_at_entry()) {
            return; // its options not valued yet
        }
        const double pnl = holding()->summary.pnl - pnl_at_entry_;
        const double take_profit = setting_.take_profit;
        const double stop_loss = setting_.stop_loss;
        const bool profit = take_profit > 0.0 && premium_ > 0.0 && pnl >= take_profit * premium_;
        const bool loss = stop_loss > 0.0 && premium_ > 0.0 && pnl <= -stop_loss * premium_;
        const bool exit = setting_.hold == 0 && minute >= setting_.exit_minute;
        if (exit || profit || loss) {
            write_log(std::format("Minute {}: closes ({}), P&L {:.2f}", minute,
                                  profit ? "take profit" : loss ? "stop loss" : "exit minute", pnl));
            if (exit && !profit && !loss && setting_.close_itm_only != 0) {
                close_in_the_money(minute);
            } else {
                close_all_strategy_positions();
            }
            done_ = true;
        }
        return;
    }
    if (!done_ && minute >= setting_.entry_minute && minute < setting_.exit_minute) {
        if (filters_allow(minute)) {
            enter(minute);
        }
    }
}

REGISTER_STRATEGY(ComboStrategy);

} // namespace strategy
