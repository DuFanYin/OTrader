#pragma once

/**
 * The first strategies to test on SPY 0DTE: each buys one
 * option a day on the day's expiry, sized to a premium budget, and sells it before the close.
 * Losses have a floor (the premium); nothing is sold short.
 *
 *  - LateDayMomentumStrategy: the session's first half hour's move (open to 10:00) predicts the
 *    last half hour's (Gao, Han, Li, Zhou, JFE 2018, who measure from the previous close); at
 *    15:30 it buys the call or the put the move points to.
 *  - OpeningRangeBreakoutStrategy: once the underlying leaves the first range_minutes' range, it
 *    buys the side it broke out to.
 *  - RandomBuyStrategy: the control: at a time and on a side drawn from the day and a seed, it
 *    buys and holds hold_minutes. What a buyer pays with no edge: every strategy must beat it.
 *
 * Times are minutes after the 09:30 New York open of the bar's day (the backtest's bars end on the
 * minute, so 09:31 is minute 1).
 */

#include "template.hpp"

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace strategy {

/** What the three share: the day's 0DTE chain, the underlying's open and range, one option bought
 * at the money (otm_steps strikes out), sized to premium_budget, and sold at exit_minute (or when
 * exit_due says). */
class ZeroDteBuyer : public OptionStrategyTemplate {
  public:
    ZeroDteBuyer(StrategyContext* context, const std::string& strategy_name,
                 const std::string& portfolio_name,
                 const std::unordered_map<std::string, double>& setting);

    void on_init_logic() override;
    void on_stop_logic() override;
    void on_timer_logic() override;

  protected:
    /** The side to buy now (CALL or PUT), if any; called each bar until it has bought. */
    virtual std::optional<model::OptionType> entry_signal(int minute) = 0;
    /** Sell what it bought now; default: from exit_minute. */
    virtual bool exit_due(int minute) const { return minute >= exit_minute_; }

    /** The underlying's price now, and at the day's first bar; high and low so far. */
    [[nodiscard]] double price() const { return price_; }
    [[nodiscard]] double open_price() const { return open_price_; }
    [[nodiscard]] double high() const { return high_; }
    [[nodiscard]] double low() const { return low_; }
    [[nodiscard]] std::chrono::year_month_day day() const { return day_; }
    [[nodiscard]] int entered_minute() const { return entered_minute_; }
    /** It saw the day's first minutes (an engine started later does not know the open). */
    [[nodiscard]] bool saw_open() const { return first_minute_ >= 1 && first_minute_ <= 5; }

  private:
    /** A new day: its 0DTE chain, and nothing held from the day before. */
    void start_day(std::chrono::year_month_day day);
    void buy(model::OptionType side, int minute);
    [[nodiscard]] bool holds_options() const;

    double premium_budget_;
    int otm_steps_;
    int exit_minute_;
    std::chrono::year_month_day day_{};
    std::string chain_symbol_;
    double price_ = 0.0;
    double open_price_ = 0.0;
    double high_ = 0.0;
    double low_ = 0.0;
    int first_minute_ = -1;   // the day's first bar it saw
    int entered_minute_ = -1; // the minute it bought today; -1: not yet
    bool done_ = false;       // bought and sold today, or gave up
};

class LateDayMomentumStrategy final : public ZeroDteBuyer {
  public:
    LateDayMomentumStrategy(StrategyContext* context, const std::string& strategy_name,
                            const std::string& portfolio_name,
                            const std::unordered_map<std::string, double>& setting);
    static std::span<const StrategyParam> parameters();

  protected:
    std::optional<model::OptionType> entry_signal(int minute) override;

  private:
    int signal_minutes_;
    int entry_minute_;
    double min_move_bp_;
    std::chrono::year_month_day signal_day_{};
    double signal_ = 0.0; // the first signal_minutes' return; 0 until then
};

class OpeningRangeBreakoutStrategy final : public ZeroDteBuyer {
  public:
    OpeningRangeBreakoutStrategy(StrategyContext* context, const std::string& strategy_name,
                                 const std::string& portfolio_name,
                                 const std::unordered_map<std::string, double>& setting);
    static std::span<const StrategyParam> parameters();

  protected:
    std::optional<model::OptionType> entry_signal(int minute) override;

  private:
    int range_minutes_;
    int last_entry_minute_;
    double range_high_ = 0.0;
    double range_low_ = 0.0;
};

class RandomBuyStrategy final : public ZeroDteBuyer {
  public:
    RandomBuyStrategy(StrategyContext* context, const std::string& strategy_name,
                      const std::string& portfolio_name,
                      const std::unordered_map<std::string, double>& setting);
    static std::span<const StrategyParam> parameters();

  protected:
    std::optional<model::OptionType> entry_signal(int minute) override;
    bool exit_due(int minute) const override;

  private:
    /** The day's draw: when, and which side. */
    void draw(std::chrono::year_month_day day);

    int seed_;
    int earliest_minute_;
    int latest_minute_;
    int hold_minutes_;
    std::chrono::year_month_day drawn_for_{};
    int draw_minute_ = -1;
    model::OptionType draw_side_ = model::OptionType::CALL;
};

} // namespace strategy
