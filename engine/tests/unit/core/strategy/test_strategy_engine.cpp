/**
 * The StrategyEngine reports a strategy in error, removes only a strategy there is with nothing
 * held or working, drives its callbacks, and, approving only some (a live account's), refuses the
 * others. What its strategies want streamed is each one's chains and the options held or ordered
 * today, one sorted set; a strategy finds only a chain it subscribes.
 */
#include "tests/lone_backtest.hpp"
#include "core/strategy/strategy_engine.hpp"
#include "model/symbol.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/strategy.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
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

/** Counts its stops, and throws on init or stop when told to. */
class EngineStepsStrategy final : public strategy::Strategy {
  public:
    using Strategy::Strategy;

    void on_init() override {
        if (throw_on_init) {
            throw std::runtime_error("no chain");
        }
    }
    void on_stop() override {
        ++stops;
        if (throw_on_stop) {
            throw std::runtime_error("cannot close");
        }
    }
    void on_timer() override {}

    bool throw_on_init = false;
    bool throw_on_stop = false;
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

TEST_F(StrategyEngineTest, AStrategysValuesAreTakenOnceTheLastWrittenStanding) {
    strategy::Strategy* s = se->find_strategy(name_);
    s->write_value("a", 1.0);
    s->write_value("a", 2.0);
    s->write_value("b", 3.0);
    EXPECT_EQ(se->take_values(name_), (std::map<std::string, double, std::less<>>{{"a", 2.0},
                                                                                {"b", 3.0}}));
    EXPECT_TRUE(se->take_values(name_).empty());
    EXPECT_TRUE(se->take_values("none").empty());
}

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

TEST_F(StrategyEngineTest, AStrategyWhoseStopThrowsIsInErrorAndAHaltStillStopsTheOthers) {
    const std::string first =
        se->add_strategy({.class_name = "EngineStepsStrategy", .portfolio = "SPY"});
    const std::string second =
        se->add_strategy({.class_name = "EngineStepsStrategy", .portfolio = "SPY"});
    static_cast<EngineStepsStrategy*>(se->find_strategy(first))->throw_on_stop = true;
    for (const std::string& name : {first, second}) {
        se->init_strategy(name);
        se->start_strategy(name);
    }
    EXPECT_NO_THROW(main.halt("test"));
    EXPECT_EQ(se->find_record(first)->state, model::StrategyState::Error);
    EXPECT_NE(se->error(first).find("cannot close"), std::string::npos);
    EXPECT_EQ(se->find_record(second)->state, model::StrategyState::Inited);
    EXPECT_EQ(static_cast<const EngineStepsStrategy*>(se->find_strategy(second))->stops, 1);
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

TEST_F(StrategyEngineTest, WhatIsWantedIsEachStrategysChainsAndTheOptionsHeldOrOrdered) {
    EXPECT_TRUE(se->wanted()->subscribed.empty());
    const std::string other =
        se->add_strategy({.class_name = "EngineProbeStrategy", .portfolio = "SPY"});
    const std::uint64_t changes = se->wanted_changes();
    se->subscribe_chains(name_, std::vector<std::string>{"SPY_20991218"});
    se->subscribe_chains(other, std::vector<std::string>{"SPY_20991218", "SPY_20991219"});
    EXPECT_EQ(se->wanted()->subscribed,
              (std::vector<std::string>{"SPY_20991218", "SPY_20991219"})); // each once
    EXPECT_GT(se->wanted_changes(), changes);

    // Found only while subscribed, and listed.
    const strategy::Strategy& probe = *se->find_strategy(name_);
    EXPECT_TRUE(probe.find_chain("SPY_20991218"));
    EXPECT_FALSE(se->find_strategy(other)->find_chain("SPY_20991219")); // not listed
    se->subscribe_chains(name_, std::vector<std::string>{"SPY_20991219"});
    EXPECT_FALSE(probe.find_chain("SPY_20991218")); // no longer subscribed

    se->set_held_options(std::vector<std::string>{"SPY-20991218-C-510", "SPY-20991218-C-500"});
    model::OrderRequest single;
    single.legs = {{.ratio = 1, .symbol = "SPY-20991218-C-500"}}; // held already: once
    se->note_ordered(single);
    model::OrderRequest spread;
    spread.legs = {{.ratio = 1, .symbol = "SPY-20991220-P-495"},
                   {.ratio = 1, .symbol = "SPY-20991218-C-505"}};
    se->note_ordered(spread);
    const std::uint64_t ordered = se->wanted_changes();
    model::OrderRequest shares;
    shares.legs = {{.ratio = 1, .symbol = "SPY"}};
    se->note_ordered(shares); // not an option
    se->note_ordered(single); // ordered already
    EXPECT_EQ(se->wanted_changes(), ordered); // nothing new
    EXPECT_EQ(se->wanted()->options,
              (std::vector<std::string>{"SPY-20991218-C-500", "SPY-20991218-C-505",
                                        "SPY-20991218-C-510", "SPY-20991220-P-495"}));
    EXPECT_EQ(se->wanted()->chains, // and the options' chains, as a backtest's day loads them
              (std::vector<std::string>{"SPY_20991218", "SPY_20991219", "SPY_20991220"}));

    se->clear_ordered();
    EXPECT_EQ(se->wanted()->options,
              (std::vector<std::string>{"SPY-20991218-C-500", "SPY-20991218-C-510"}));
    se->set_held_options({});
    EXPECT_TRUE(se->wanted()->options.empty());
    ASSERT_TRUE(se->remove_strategy(other)); // its chains go with it
    EXPECT_EQ(se->wanted()->subscribed, std::vector<std::string>{"SPY_20991219"});
}

} // namespace
