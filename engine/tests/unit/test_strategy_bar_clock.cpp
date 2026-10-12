/**
 * A strategy runs once per bar of event time, however often the timer ticks: the backtest ticks
 * once per bar, live once a second (EventEngine interval 1). Same data, same calls. It hears of
 * every snapshot of its portfolio (on_snapshot), before the bar's timer.
 */
#include "core/engines/option_strategy.hpp"
#include "core/engine_backtest.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/template.hpp"
#include "model/event.hpp"
#include "model/utility.hpp"
#include <chrono>
#include <gtest/gtest.h>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono;
using model::DateTime;
using model::Event;
using model::EventType;

// 2025-03-03 09:30 New York (EST).
const DateTime kOpen = sys_days{2025y / March / 3} + 14h + 30min;
constexpr int kBars = 5;

class ClockProbeStrategy final : public strategy::OptionStrategyTemplate {
  public:
    using OptionStrategyTemplate::OptionStrategyTemplate;

    void on_init_logic() override {}
    void on_stop_logic() override {}
    void on_timer_logic() override { seen_.push_back(now()); }
    void on_snapshot() override { snapshots_.push_back(now()); }

    const std::vector<DateTime>& seen() const { return seen_; }
    const std::vector<DateTime>& snapshots() const { return snapshots_; }

  private:
    std::vector<DateTime> seen_;
    std::vector<DateTime> snapshots_;
};

REGISTER_STRATEGY(ClockProbeStrategy);

// The strategy after kBars one-minute snapshots from the open, with `ticks_per_bar` timer events
// after each and one before the first: the event times on_timer_logic and on_snapshot saw.
auto run_probe(int ticks_per_bar, int timer_trigger = 1)
    -> std::pair<std::vector<DateTime>, std::vector<DateTime>> {
    core::backtest::MainEngine main;
    main.portfolio_structure()->ensure_portfolio("backtest");
    core::OptionStrategyEngine* se = main.option_strategy_engine();
    const std::string name =
        se->add_strategy("ClockProbeStrategy", "backtest", {{"timer_trigger", timer_trigger}});
    se->init_strategy(name);
    se->start_strategy(name);

    main.put_event(Event(EventType::Timer)); // no snapshot yet: nothing to run on
    for (int bar = 0; bar < kBars; ++bar) {
        model::PortfolioSnapshot* snap = main.acquire_snapshot();
        snap->portfolio_name = "backtest";
        snap->datetime = kOpen + minutes(bar);
        main.put_event(Event(EventType::Snapshot, snap));
        for (int tick = 0; tick < ticks_per_bar; ++tick) {
            main.put_event(Event(EventType::Timer));
        }
    }
    const auto* probe = static_cast<const ClockProbeStrategy*>(se->get_strategy(name));
    return {probe->seen(), probe->snapshots()};
}

auto run(int ticks_per_bar, int timer_trigger = 1) -> std::vector<DateTime> {
    return run_probe(ticks_per_bar, timer_trigger).first;
}

TEST(StrategyBarClock, RunsOncePerBarHoweverOftenTheTimerTicks) {
    std::vector<DateTime> bars;
    for (int bar = 0; bar < kBars; ++bar) {
        bars.push_back(kOpen + minutes(bar));
    }
    const std::vector<DateTime> backtest = run(1);
    EXPECT_EQ(backtest, bars);
    EXPECT_EQ(run(60), backtest); // live: a tick a second
}

TEST(StrategyBarClock, HearsOfEverySnapshot) {
    std::vector<DateTime> bars;
    for (int bar = 0; bar < kBars; ++bar) {
        bars.push_back(kOpen + minutes(bar));
    }
    EXPECT_EQ(run_probe(1).second, bars);
    EXPECT_EQ(run_probe(60, 2).second, bars); // whatever the timer does
}

TEST(StrategyBarClock, TimerTriggerCountsBars) {
    const std::vector<DateTime> every_other = run(1, 2);
    EXPECT_EQ(every_other, (std::vector<DateTime>{kOpen + 1min, kOpen + 3min}));
    EXPECT_EQ(run(60, 2), every_other);
}

TEST(StrategyBarClock, MinutesSinceOpenFollowsNewYorkTime) {
    EXPECT_EQ(model::minutes_since_open(kOpen), 0);
    EXPECT_EQ(model::minutes_since_open(kOpen - 1min), -1);
    EXPECT_EQ(model::minutes_since_open(kOpen + 15min + 59s), 15);
    // Summer (EDT): the open is 13:30 UTC.
    EXPECT_EQ(model::minutes_since_open(sys_days{2025y / August / 4} + 13h + 30min), 0);
}

} // namespace
