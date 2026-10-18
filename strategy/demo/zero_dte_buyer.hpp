#pragma once

/**
 * ZeroDteBuyer: base for buying one 0DTE option a day, or a debit spread (spread_width > 0: the
 * option, less one spread_width strikes further out of the money), sized to a premium budget and
 * sold before the close (loss floored at the premium). Subclasses give entry_signal and optionally
 * exit_due.
 * Header only, so strategies built as their own library can include it.
 *
 * Minutes count from the 09:30 New York open; bars end on the minute, so 09:31 is minute 1.
 */

#include "strategy.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <vector>

namespace strategy {

class ZeroDteBuyer : public Strategy {
  public:
    struct Setting {
        Param<double> premium_budget{"premium_budget", 1500, 1, 1'000'000}; // USD per trade
        Param<int> otm_steps{"otm_steps", 0, 0, 20};       // strikes out of the money
        Param<int> exit_minute{"exit_minute", 380, 1, 390}; // 15:50
        Param<int> spread_width{"spread_width", 0, 0, 20};  // strikes; 0: the option alone
    };
    using Strategy::Strategy;

    void on_init() override;
    void on_day() override;
    void on_timer() override;

  protected:
    /** The side to buy now (CALL or PUT), if any; called each bar until it has bought. */
    virtual std::optional<model::OptionType> entry_signal(int minute) = 0;
    /** Default: from exit_minute. */
    virtual bool exit_due(int minute) const { return minute >= setting_.exit_minute; }

    /** The underlying's price now, and at the day's first bar; high and low so far. */
    [[nodiscard]] double price() const { return price_; }
    [[nodiscard]] double open_price() const { return open_price_; }
    [[nodiscard]] double high() const { return high_; }
    [[nodiscard]] double low() const { return low_; }
    /** The underlying's last price the day before; 0 when it saw none. */
    [[nodiscard]] double previous_close() const { return previous_close_; }
    [[nodiscard]] int entered_minute() const { return entered_minute_; }
    /** False when the engine started late and never saw the open. */
    [[nodiscard]] bool saw_open() const { return first_minute_ >= 1 && first_minute_ <= 5; }

  private:
    void buy(model::OptionType side, int minute);

    const Setting setting_ = read_setting<Setting>();
    std::string chain_symbol_;
    double price_ = 0.0;
    double open_price_ = 0.0;
    double high_ = 0.0;
    double low_ = 0.0;
    double previous_close_ = 0.0;
    int first_minute_ = -1;   // the day's first bar it saw
    int entered_minute_ = -1; // the minute it bought today; -1: not yet
    bool done_ = false;       // bought and sold today, or gave up
};

inline void ZeroDteBuyer::on_init() {
    write_log(std::format("Buys one 0DTE option a day for up to {:.0f} USD, sold by minute {}",
                          setting_.premium_budget, setting_.exit_minute));
}

inline void ZeroDteBuyer::on_day() {
    previous_close_ = price_;
    price_ = open_price_ = high_ = low_ = 0.0;
    first_minute_ = -1;
    entered_minute_ = -1;
    done_ = false;
    chain_symbol_ = select_chain(0, 0);
    if (chain_symbol_.empty()) {
        write_log(std::format("{}: no chain expires today: no fill", day()), utilities::WARNING);
        done_ = true;
    }
    // Started again during the day: what it did before is in its holding, not in its members.
    if (holds_options()) {
        entered_minute_ = std::max(last_order_minute(), 0); // when it bought, as before
        write_log(std::format("Holds what it bought at minute {}: sold as planned",
                              entered_minute_));
    } else if (traded_today()) {
        done_ = true;
        write_log("Bought and sold today already: no second trade");
    }
}

inline void ZeroDteBuyer::on_timer() {
    const int minute = this->minute();
    const std::optional<model::Underlying> u = underlying();
    if (!u || minute < 1 || minute > 390) {
        return;
    }
    const double mid = u->bid() > 0.0 && u->ask() > 0.0 ? 0.5 * (u->bid() + u->ask()) : u->mid();
    if (mid <= 0.0) {
        return;
    }
    price_ = mid;
    if (open_price_ <= 0.0) {
        open_price_ = high_ = low_ = mid;
        first_minute_ = minute;
    }
    high_ = std::max(high_, mid);
    low_ = std::min(low_, mid);

    if (entered_minute_ >= 0 && !done_) {
        if (exit_due(minute) && holds_options() && !has_active_order()) {
            write_log(std::format("Minute {}: selling what it bought at minute {}", minute,
                                  entered_minute_));
            close_all_strategy_positions();
            done_ = true;
        }
        return;
    }
    if (done_ || has_active_order() || minute >= setting_.exit_minute) {
        return;
    }
    if (const std::optional<model::OptionType> side = entry_signal(minute)) {
        buy(*side, minute);
    }
}

inline void ZeroDteBuyer::buy(model::OptionType side, int minute) {
    const std::optional<model::Chain> chain = find_chain(chain_symbol_);
    // otm_steps out of the money; none yet (no quotes or strike): retry next bar.
    const std::optional<model::Option> option =
        chain ? option_at(*chain, side,
                          side == model::OptionType::CALL ? setting_.otm_steps.value()
                                                          : -setting_.otm_steps.value())
              : std::nullopt;
    if (!option || option->ask() <= 0.0) {
        return;
    }
    std::optional<model::Option> sold;
    double cost = option->ask();
    if (setting_.spread_width > 0) {
        const int out = setting_.otm_steps + setting_.spread_width;
        sold = option_at(*chain, side, side == model::OptionType::CALL ? out : -out);
        if (!sold || sold->bid() <= 0.0) {
            return;
        }
        cost = std::max(option->ask() - sold->bid(), 0.01);
    }
    const double multiplier = option->multiplier() > 0.0 ? option->multiplier() : 100.0;
    const double contracts = std::floor(setting_.premium_budget / (cost * multiplier));
    if (contracts < 1.0) {
        write_log(std::format("Minute {}: one {} costs {:.0f} USD, over the budget: no fill",
                              minute, option->symbol(), cost * multiplier),
                  utilities::WARNING);
        done_ = true;
        return;
    }
    const std::string order_id =
        !sold
            ? send_order(std::array{leg(option->symbol(), model::Side::BUY)}, 0.0, contracts)
            : send_order(combo_legs(model::ComboType::SPREAD,
                                    {{"long_leg", option->symbol()}, {"short_leg", sold->symbol()}},
                                    model::Side::BUY),
                         0.0, contracts, model::OrderType::MARKET, model::ComboType::SPREAD);
    if (order_id.empty()) {
        done_ = true; // refused (the risk check says why): no second try today
        return;
    }
    entered_minute_ = minute;
    write_log(std::format("Minute {}: buys {} {} {}{} at about {:.2f} (underlying {:.2f}, open "
                          "{:.2f})",
                          minute, contracts, side == model::OptionType::CALL ? "call" : "put",
                          option->symbol(), sold ? " less " + sold->symbol() : "", cost,
                          price_, open_price_));
}

} // namespace strategy
