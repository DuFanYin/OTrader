/**
 * The StrategyEngine reports a strategy in error, removes only a strategy there is with nothing
 * held or working, drives its callbacks, and, approving only some (a live account's), refuses the
 * others.
 */
#include "tests/lone_backtest.hpp"
#include "core/strategy/strategy_engine.hpp"
#include "model/symbol.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/strategy.hpp"
#include <algorithm>
#include <chrono>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono;
using model::Side;

class EngineProbeStrategy final : public strategy::Strategy {
  public:
    using Strategy::Strategy;

    void on_timer() override {}
};

REGISTER_STRATEGY(EngineProbeStrategy);

/** Counts the days and the snapshots it hears of. */
class EngineCountingStrategy final : public strategy::Strategy {
  public:
    using Strategy::Strategy;

    void on_day() override { ++days; }
    void on_timer() override {}
    void on_snapshot() override { ++snapshots; }

    int days = 0;
    int snapshots = 0;
};

REGISTER_STRATEGY(EngineCountingStrategy);

/** Counts its stops, and throws on init when told to. */
class EngineStepsStrategy final : public strategy::Strategy {
  public:
    using Strategy::Strategy;

    void on_init() override {
        if (throw_on_init) {
            throw std::runtime_error("no chain");
        }
    }
    void on_stop() override { ++stops; }
    void on_timer() override {}

    bool throw_on_init = false;
    int stops = 0;
};

REGISTER_STRATEGY(EngineStepsStrategy);

class StrategyEngineTest : public ::testing::Test {
  protected:
    void SetUp() override {
        model::Portfolios& s = *main.portfolios();
        s.ensure_portfolios_created();
        model::Contract c;
        c.symbol = call_;
        c.product = model::Product::OPTION;
        c.multiplier = 100;
        c.min_quantity = 1;
        c.trading_class = "SPY";
        s.load_contracts(std::vector{c});
        name_ = se->add_strategy({.class_name = "EngineProbeStrategy", .portfolio = "SPY"});
    }

    const std::string call_ = model::option_symbol(
        {.root = "SPY", .expiry = 2099y / December / 18, .type = model::OptionType::CALL,
         .strike = 600});
    test::LoneBacktest backtest;
    core::Main& main = backtest.main();
    core::StrategyEngine* se = main.strategy_engine();
    std::string name_;
};

TEST_F(StrategyEngineTest, AStrategyInErrorIsReportedAndRunsNoMore) {
    const std::string other =
        se->add_strategy({.class_name = "EngineProbeStrategy", .portfolio = "SPY"});
    se->init_strategy(name_);
    se->start_strategy(name_);
    EXPECT_TRUE(se->errors().empty());
    se->find_strategy(name_)->set_error("its callback failed");
    EXPECT_EQ(se->errors(),
              (std::vector<std::pair<std::string, std::string>>{{name_, "its callback failed"}}));
    EXPECT_EQ(se->find_record(name_)->state, model::StrategyState::Error);
    EXPECT_TRUE(se->error(other).empty());
}

TEST_F(StrategyEngineTest, AStrategyGoesOnlyThroughTheStepsItsStateAllows) {
    using model::StrategyState;
    const std::string name =
        se->add_strategy({.class_name = "EngineStepsStrategy", .portfolio = "SPY"});
    const auto* s = static_cast<const EngineStepsStrategy*>(se->find_strategy(name));
    const auto state = [&] { return se->find_record(name)->state; };
    EXPECT_EQ(state(), model::StrategyState::Added);
    EXPECT_THROW(se->start_strategy(name), std::logic_error); // not inited
    EXPECT_THROW(se->stop_strategy(name), std::logic_error);  // not running

    se->init_strategy(name);
    EXPECT_EQ(state(), model::StrategyState::Inited);
    EXPECT_THROW(se->init_strategy(name), std::logic_error); // inited already
    se->start_strategy(name);
    EXPECT_EQ(state(), model::StrategyState::Running);
    EXPECT_THROW(se->start_strategy(name), std::logic_error); // running already
    se->stop_strategy(name);
    EXPECT_EQ(state(), model::StrategyState::Inited);
    EXPECT_EQ(s->stops, 1);
    EXPECT_THROW(se->stop_strategy(name), std::logic_error);
    EXPECT_EQ(s->stops, 1) << "a strategy not running does not run its stop";

    se->start_strategy(name);
    se->set_error(name, "broke");
    se->set_error(name, "broke again");
    EXPECT_EQ(state(), model::StrategyState::Error);
    EXPECT_EQ(se->error(name), "broke") << "the first cause stays";
    EXPECT_THROW(se->start_strategy(name), std::logic_error); // in error: init again first
    se->init_strategy(name);
    EXPECT_EQ(state(), model::StrategyState::Inited);
    EXPECT_TRUE(se->error(name).empty());
}

TEST_F(StrategyEngineTest, AStrategyWhoseInitThrowsIsInError) {
    const std::string name =
        se->add_strategy({.class_name = "EngineStepsStrategy", .portfolio = "SPY"});
    static_cast<EngineStepsStrategy*>(se->find_strategy(name))->throw_on_init = true;
    EXPECT_THROW(se->init_strategy(name), std::runtime_error);
    EXPECT_EQ(se->find_record(name)->state, model::StrategyState::Error);
    EXPECT_NE(se->error(name).find("no chain"), std::string::npos);
}

TEST_F(StrategyEngineTest, AStrategyNeedsItsPortfolio) {
    EXPECT_THROW(se->add_strategy({.class_name = "EngineProbeStrategy",
                                   .portfolio = "NoSuchPortfolio"}), std::runtime_error);
}

TEST_F(StrategyEngineTest, OnlyAStrategyThereIsWithNothingWorkingIsRemoved) {
    EXPECT_FALSE(se->remove_strategy("NoSuchStrategy_SPY"));
    model::Order working;
    working.order_id = "1";
    working.symbol = call_;
    working.legs = {{.ratio = 1, .symbol = call_}};
    working.status = model::Status::WORKING;
    main.bookkeeping_engine()->add_order(name_, working);
    EXPECT_THROW(se->remove_strategy(name_), std::runtime_error);

    working.status = model::Status::CANCELLED;
    main.bookkeeping_engine()->update_order(working, {});
    EXPECT_TRUE(se->remove_strategy(name_));
    EXPECT_EQ(se->find_strategy(name_), nullptr);
    EXPECT_EQ(main.bookkeeping_engine()->find_holding(name_), nullptr);
}

TEST_F(StrategyEngineTest, AStrategyThereIsNotIsRefusedAndOneThereIsIsNotRestoredOverIt) {
    EXPECT_THROW(se->init_strategy("NoSuchStrategy"), std::invalid_argument);
    EXPECT_THROW(se->start_strategy("NoSuchStrategy"), std::invalid_argument);
    EXPECT_THROW(se->stop_strategy("NoSuchStrategy"), std::invalid_argument);
    EXPECT_THROW(se->restore_strategy({.name = name_,
                                       .class_name = "EngineProbeStrategy",
                                       .portfolio = "SPY"}),
                 std::runtime_error);
    EXPECT_EQ(se->strategies().size(), 1U);
}

TEST_F(StrategyEngineTest, ApprovingOnlySomeRefusesTheOthersAddedOrRestored) {
    // As a live account's list: the class and its resolved setting (timer_trigger 5, not 1).
    se->approve_only([](const model::StrategyRecord& r) {
        return r.class_name == "EngineProbeStrategy" && r.setting.at("timer_trigger") == 5;
    });
    EXPECT_NO_THROW(se->add_strategy({.class_name = "EngineProbeStrategy",
                                      .portfolio = "SPY",
                                      .setting = {{"timer_trigger", 5}}}));
    EXPECT_THROW(se->add_strategy({.class_name = "EngineProbeStrategy",
                                   .portfolio = "SPY"}), std::runtime_error);
    EXPECT_THROW(se->add_strategy({.class_name = "EngineCountingStrategy",
                                   .portfolio = "SPY",
                                   .setting = {{"timer_trigger", 5}}}),
                 std::runtime_error);
    EXPECT_THROW(se->restore_strategy({.name = "saved_before",
                                       .class_name = "EngineCountingStrategy",
                                       .portfolio = "SPY"}),
                 std::runtime_error);
    EXPECT_EQ(se->strategies().size(), 2U); // the one added before, and the approved one
}

TEST_F(StrategyEngineTest, AStrategyHoldingPositionsIsNotRemoved) {
    model::Fill bought;
    bought.fill_id = "1";
    bought.order_id = "1";
    bought.symbol = call_;
    bought.side = Side::BUY;
    bought.price = 1.0;
    bought.quantity = 1;
    main.bookkeeping_engine()->add_order(name_, {.symbol = call_,
                                          .order_id = "1",
                                          .status = model::Status::FILLED,
                                          .legs = {{.ratio = 1, .symbol = call_}}});
    main.bookkeeping_engine()->book_fill(bought);
    EXPECT_THROW(se->remove_strategy(name_), std::runtime_error);
    EXPECT_NE(se->find_strategy(name_), nullptr);
}

TEST_F(StrategyEngineTest, SnapshotsAndDaysReachOnlyTheirPortfoliosStrategiesAsTheyRun) {
    const auto counting = [&](const std::string& portfolio) {
        const std::string name =
            se->add_strategy({.class_name = "EngineCountingStrategy", .portfolio = portfolio});
        se->init_strategy(name);
        return static_cast<EngineCountingStrategy*>(se->find_strategy(name));
    };
    EngineCountingStrategy* running = counting("SPY");
    se->start_strategy(running->strategy_name());
    EngineCountingStrategy* inited = counting("SPY");
    EngineCountingStrategy* elsewhere = counting("SPXW");
    se->start_strategy(elsewhere->strategy_name());
    EXPECT_EQ(running->days, 1); // at init

    se->on_snapshot("SPY");
    EXPECT_EQ(running->snapshots, 1);
    EXPECT_EQ(inited->snapshots, 0);    // not started
    EXPECT_EQ(elsewhere->snapshots, 0); // another portfolio's
    running->set_error("failed");
    se->on_snapshot("SPY");
    EXPECT_EQ(running->snapshots, 1);

    se->on_day("SPY"); // the same day's contracts again
    EXPECT_EQ(inited->days, 1);
    main.portfolios()->find_portfolio("SPY")->set_trading_day(sys_days{2099y / March / 2});
    se->on_day("SPY");
    EXPECT_EQ(inited->days, 2);
    EXPECT_EQ(running->days, 1); // in error
    EXPECT_EQ(elsewhere->days, 1);

    se->on_timer();
    const std::string report = se->callback_report();
    // A line per strategy timed: elsewhere, never quoted, has had no bar to run on.
    EXPECT_EQ(std::ranges::count(report, '\n'), 2) << report;
}

} // namespace
