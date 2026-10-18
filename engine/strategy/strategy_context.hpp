#pragma once

/**
 * The engine as one strategy sees it: bound to that strategy (its record, its portfolio, its
 * holding) by whoever hosts it, core::StrategyEngine in backtest and live; tests may give their
 * own. A strategy never names itself to it.
 */

#include "model/daily_table.hpp"
#include "model/object.hpp"
#include "model/portfolio.hpp"
#include "utilities/log_level.hpp"
#include <span>
#include <string>

namespace strategy {

class StrategyContext {
  public:
    virtual ~StrategyContext() = default;

    // ---- What the strategy sees
    /** Its name, class, portfolio and setting (resolved: every parameter its class declares). */
    virtual const model::StrategyRecord& record() const = 0;
    virtual const model::Portfolio& portfolio() const = 0;
    /** What it holds: kept up to date by the engine, from its fills. */
    virtual const model::StrategyHolding& holding() const = 0;
    /** True while an order it sent is still working. */
    virtual bool has_active_order() const = 0;
    /** Values by day (data/daily): empty when none were given. */
    virtual const model::DailyTable& daily() const = 0;

    // ---- What it may do
    /** The order's id; empty when not sent (refused or rejected). */
    virtual std::string send_order(const model::OrderRequest& request) = 0;
    /** Keep its delta near delta_target (within delta_range), every timer_trigger bars. */
    virtual void register_hedging(int timer_trigger, int delta_target, int delta_range) = 0;
    virtual void unregister_hedging() = 0;
    /** Live quotes for these chains in place of those before, until it is removed. */
    virtual void subscribe_chains(std::span<const std::string> chains) = 0;
    virtual void write_log(const std::string& msg, utilities::LogLevel level) const = 0;
    /** It stops: in error, its positions left as they are, until it is inited again. The first
     * cause stays. */
    virtual void set_error(const std::string& msg) = 0;
};

} // namespace strategy
