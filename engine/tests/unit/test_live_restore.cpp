/**
 * The live engine starts where it stopped: a strategy comes back from the database with every
 * order and fill saved booked again (one IB reports again is dropped), and cannot be removed while
 * it holds anything. An
 * option held past its close with no closing price to settle at keeps its strategy from starting
 * until it is settled by hand; the settlement (the option closed, its shares delivered) is booked
 * and saved like any fill, so the next start rebooks it too. A name, which its fills are kept under, is a
 * strategy's for good. The record of orders and fills is emptied only when nothing is held.
 * The risk limits and a halt outlive a restart, and no strategy starts while halted; a fill booked
 * by hand is held and rebooked like any.
 */
#include "core/engine_live.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/template.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include <chrono>
#include <sqlite3.h>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono;
using model::Direction;

class LiveRestore : public ::testing::Test {
  protected:
    void SetUp() override {
        path_ = ::testing::TempDir() + "optra_" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".db";
        remove_files();
        setenv("OPTRA_DB", path_.c_str(), 1);
    }
    void TearDown() override {
        unsetenv("OPTRA_DB");
        remove_files();
    }
    void remove_files() const {
        for (const char* suffix : {"", "-wal", "-shm"}) {
            std::filesystem::remove(path_ + suffix);
        }
    }

    std::string path_;
};

/** Wait (up to 5 s) until done() holds. */
auto eventually(const std::function<bool()>& done) -> bool {
    for (int i = 0; i < 500; ++i) {
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(milliseconds(10));
    }
    return done();
}

/** A fill of `volume` `symbol` for the strategy, as the gateway reports it. */
void fill(core::live::MainEngine& engine, const std::string& strategy, const std::string& id,
          const std::string& symbol, double volume, double price,
          Direction direction = Direction::LONG) {
    // The trading state is the event thread's: changed and read there only.
    engine.on_event_thread([&] { engine.execution_engine()->register_active_order(strategy, id); });
    model::OrderData order;
    order.orderid = id;
    order.symbol = symbol;
    order.direction = direction;
    order.type = model::OrderType::LIMIT;
    order.price = price;
    order.volume = volume;
    order.traded = volume;
    order.status = model::Status::ALLTRADED;
    model::TradeData trade;
    trade.tradeid = "T" + id;
    trade.orderid = id;
    trade.symbol = symbol;
    trade.direction = direction;
    trade.price = price;
    trade.volume = volume;
    model::OrderData* o = engine.acquire_order();
    *o = order;
    engine.put_event(model::Event(model::EventType::Order, o));
    model::TradeData* t = engine.acquire_trade();
    *t = trade;
    engine.put_event(model::Event(model::EventType::Trade, t));
}

/** Notes the thread each call from the engine comes on. */
class ThreadProbeStrategy final : public strategy::OptionStrategyTemplate {
  public:
    using OptionStrategyTemplate::OptionStrategyTemplate;

    void on_init_logic() override { seen_.push_back(std::this_thread::get_id()); }
    void on_stop_logic() override {}
    void on_timer_logic() override { seen_.push_back(std::this_thread::get_id()); }
    void on_order(const model::OrderData& order) override {
        seen_.push_back(std::this_thread::get_id());
        OptionStrategyTemplate::on_order(order);
    }
    void on_trade(const model::TradeData& trade) override {
        seen_.push_back(std::this_thread::get_id());
        OptionStrategyTemplate::on_trade(trade);
    }

    const std::vector<std::thread::id>& seen() const { return seen_; }

  private:
    std::vector<std::thread::id> seen_;
};

REGISTER_STRATEGY(ThreadProbeStrategy);

/** Counts the calls of on_stop, in every instance. */
class StopCountStrategy final : public strategy::OptionStrategyTemplate {
  public:
    using OptionStrategyTemplate::OptionStrategyTemplate;

    void on_init_logic() override {}
    void on_stop_logic() override { ++stops; }
    void on_timer_logic() override {}

    static inline int stops = 0;
};

REGISTER_STRATEGY(StopCountStrategy);

auto quantity(core::live::MainEngine& engine, const std::string& strategy, const std::string& option)
    -> int {
    return engine.on_event_thread([&] {
        const model::StrategyHolding* h = engine.position_engine()->find_holding(strategy);
        if (h == nullptr) {
            return -1;
        }
        const auto it = h->optionPositions.find(option);
        return it != h->optionPositions.end() ? it->second.quantity : 0;
    });
}

TEST_F(LiveRestore, AStrategyComesBackWithItsHolding) {
    const std::string option = model::option_symbol({.root = "SPY",
                                                         .expiry = 2099y / December / 18,
                                                         .type = model::OptionType::CALL,
                                                         .strike = 600});
    std::string name;
    {
        core::live::MainEngine engine;
        name = engine.add_strategy("StraddleTestStrategy", "SPY", {});
        fill(engine, name, "100", option, 2, 1.5);
        ASSERT_TRUE(eventually([&] { return quantity(engine, name, option) == 2; }));
        engine.close();
    }
    core::live::MainEngine engine;
    ASSERT_NE(engine.option_strategy_engine()->get_strategy(name), nullptr);
    EXPECT_EQ(quantity(engine, name, option), 2);
    EXPECT_DOUBLE_EQ(
        engine.position_engine()->find_holding(name)->optionPositions.at(option).avg_cost, 1.5);
    EXPECT_EQ(engine.execution_engine()->get_strategy_name_for_order("100"), name);
    EXPECT_THROW(engine.remove_strategy(name), std::runtime_error); // it holds a call
    EXPECT_TRUE(engine.removed_strategies().empty());
    engine.close();
}

TEST_F(LiveRestore, ClosingTheEngineDoesNotStopItsStrategies) {
    // on_stop may close the strategy's positions; a restart resumes them instead.
    StopCountStrategy::stops = 0;
    std::string name;
    {
        core::live::MainEngine engine;
        name = engine.add_strategy("StopCountStrategy", "SPY", {});
        engine.init_strategy(name);
        engine.close();
    }
    EXPECT_EQ(StopCountStrategy::stops, 0);
    core::live::MainEngine engine;
    const std::vector<adapters::StrategyRecord> saved = engine.strategies();
    ASSERT_EQ(saved.size(), 1U);
    EXPECT_EQ(saved.front().state, adapters::StrategyState::Inited);
    engine.stop_strategy(name);
    EXPECT_EQ(StopCountStrategy::stops, 1);
    engine.close();
}

TEST_F(LiveRestore, ARestartBooksEveryFillSavedAndDropsOneIbReportsAgain) {
    const std::string option = model::option_symbol({.root = "SPY",
                                                         .expiry = 2099y / December / 18,
                                                         .type = model::OptionType::CALL,
                                                         .strike = 610});
    std::string name;
    {
        core::live::MainEngine engine;
        name = engine.add_strategy("StraddleTestStrategy", "SPY", {});
        fill(engine, name, "300", option, 3, 2.0);
        fill(engine, name, "301", option, 1, 1.0, Direction::SHORT);
        ASSERT_TRUE(eventually([&] { return quantity(engine, name, option) == 2; }));
        engine.close();
    }
    core::live::MainEngine engine;
    EXPECT_EQ(quantity(engine, name, option), 2);
    const auto position = [&] {
        return engine.on_event_thread([&] {
            return engine.position_engine()->find_holding(name)->optionPositions.at(option);
        });
    };
    EXPECT_DOUBLE_EQ(position().avg_cost, 2.0);
    EXPECT_DOUBLE_EQ(position().realized_pnl, -100.0); // one sold at 1.0, bought at 2.0
    // IB repeats today's fills on a connect: the one booked already is dropped.
    fill(engine, name, "300", option, 3, 2.0);
    EXPECT_TRUE(eventually([&] { return engine.on_event_thread([&] {
        return engine.execution_engine()->get_trade("T300") != nullptr; }); }));
    EXPECT_EQ(quantity(engine, name, option), 2);
    engine.close();
}

TEST_F(LiveRestore, TheTradingRecordIsEmptiedOnlyWhenNothingIsHeld) {
    const std::string option = model::option_symbol({.root = "SPY",
                                                         .expiry = 2099y / December / 18,
                                                         .type = model::OptionType::PUT,
                                                         .strike = 630});
    core::live::MainEngine engine;
    const std::string name = engine.add_strategy("StraddleTestStrategy", "SPY", {});
    fill(engine, name, "320", option, 1, 2.0);
    ASSERT_TRUE(eventually([&] { return quantity(engine, name, option) == 1; }));
    EXPECT_THROW(engine.wipe_database("trading"), std::runtime_error);
    EXPECT_THROW(engine.wipe_database("everything"), std::invalid_argument);
    EXPECT_EQ(engine.strategies_with_records(), std::vector<std::string>{name});

    fill(engine, name, "321", option, 1, 2.5, Direction::SHORT); // flat
    ASSERT_TRUE(eventually([&] { return quantity(engine, name, option) == 0; }));
    const std::vector<std::pair<std::string, int>> emptied = engine.wipe_database("trading");
    EXPECT_EQ(emptied, (std::vector<std::pair<std::string, int>>{{"orders", 2}, {"trades", 2}}));
    EXPECT_TRUE(engine.saved_orders("", 0).empty());
    EXPECT_TRUE(engine.strategies_with_records().empty());
    engine.close();
}

TEST_F(LiveRestore, ARemovedStrategyIsRestoredWithItsFills) {
    const std::string option = model::option_symbol({.root = "SPY",
                                                         .expiry = 2099y / December / 18,
                                                         .type = model::OptionType::PUT,
                                                         .strike = 500});
    core::live::MainEngine engine;
    const std::string name = engine.add_strategy("StraddleTestStrategy", "SPY", {}, "kept");
    fill(engine, name, "200", option, 1, 2.0);
    ASSERT_TRUE(eventually([&] { return quantity(engine, name, option) == 1; }));
    engine.on_event_thread( // sold at 3: flat, 100 made
        [&] { engine.execution_engine()->register_active_order(name, "201"); });
    model::TradeData* t = engine.acquire_trade();
    *t = {};
    t->tradeid = "T201";
    t->orderid = "201";
    t->symbol = option;
    t->direction = Direction::SHORT;
    t->price = 3.0;
    t->volume = 1;
    engine.put_event(model::Event(model::EventType::Trade, t));
    ASSERT_TRUE(eventually([&] { return quantity(engine, name, option) == 0; }));

    engine.remove_strategy(name);
    EXPECT_EQ(engine.option_strategy_engine()->get_strategy(name), nullptr);
    EXPECT_EQ(engine.removed_strategies(), std::vector<std::string>{name});
    engine.restore_strategy(name);
    ASSERT_NE(engine.option_strategy_engine()->get_strategy(name), nullptr);
    EXPECT_DOUBLE_EQ(
        engine.position_engine()->find_holding(name)->optionPositions.at(option).realized_pnl,
        100.0);
    EXPECT_TRUE(engine.removed_strategies().empty());
    engine.close();
}

TEST_F(LiveRestore, AnExpiredOptionWaitsForItsCloseThenSettlesInShares) {
    const auto expiry = year_month_day{floor<days>(system_clock::now()) - days(3)};
    const std::string call = model::option_symbol(
        {.root = "SPY", .expiry = expiry, .type = model::OptionType::CALL, .strike = 580});
    std::string name;
    {
        core::live::MainEngine engine;
        name = engine.add_strategy("StraddleTestStrategy", "SPY", {});
        fill(engine, name, "300", call, 1, 4.0);
        ASSERT_TRUE(eventually([&] { return quantity(engine, name, call) == 1; }));

        // The timer finds it past its close with no quote from then: it waits for a price, and
        // the strategy does not start.
        std::this_thread::sleep_for(milliseconds(2500));
        try {
            engine.start_strategy(name);
            ADD_FAILURE() << "started with an expired option unsettled";
        } catch (const std::runtime_error& e) {
            EXPECT_NE(std::string(e.what()).find("expired options"), std::string::npos) << e.what();
        }

        EXPECT_EQ(engine.settle_expired("SPY", expiry, 585.0), 2); // the call; its shares
        ASSERT_TRUE(eventually([&] {
            const auto* h = engine.position_engine()->find_holding(name);
            return quantity(engine, name, call) == 0 && h->underlyingPosition.quantity == 100;
        }));
        const auto* h = engine.position_engine()->find_holding(name);
        EXPECT_EQ(h->underlyingPosition.symbol, "SPY");
        EXPECT_DOUBLE_EQ(h->underlyingPosition.avg_cost, 580.0);            // bought at the strike
        EXPECT_DOUBLE_EQ(h->optionPositions.at(call).realized_pnl, -400.0); // the premium
        EXPECT_THROW(engine.settle_expired("QQQ", expiry, 585.0), std::invalid_argument);
        engine.close();
    }
    core::live::MainEngine engine; // the settlement was saved like any fill
    const auto* h = engine.position_engine()->find_holding(name);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(quantity(engine, name, call), 0);
    EXPECT_EQ(h->underlyingPosition.quantity, 100);
    engine.close();
}

TEST_F(LiveRestore, StrategiesRunOnTheEventThread) {
    const std::string option = model::option_symbol({.root = "SPY",
                                                         .expiry = 2099y / December / 18,
                                                         .type = model::OptionType::CALL,
                                                         .strike = 620});
    core::live::MainEngine engine;
    const std::string name = engine.add_strategy("ThreadProbeStrategy", "SPY", {});
    engine.init_strategy(name);
    fill(engine, name, "600", option, 1, 1.0);
    // Queued behind the fill's events: they have reached the strategy by then.
    const auto [event_thread, seen] = engine.on_event_thread([&] {
        const auto* probe = dynamic_cast<const ThreadProbeStrategy*>(
            engine.option_strategy_engine()->get_strategy(name));
        return std::pair{std::this_thread::get_id(), probe->seen()};
    });
    EXPECT_NE(event_thread, std::this_thread::get_id());
    ASSERT_EQ(seen.size(), 3U); // on_init, on_order, on_trade
    for (const std::thread::id id : seen) {
        EXPECT_EQ(id, event_thread);
    }
    engine.close();
}

TEST_F(LiveRestore, ACancelGoesToTheGateway) {
    // It once came back to the engine, round and round until the stack ran out.
    core::live::MainEngine engine;
    model::CancelRequest req;
    req.orderid = "1";
    req.symbol = "SPY";
    engine.on_event_thread([&] { engine.cancel_order(req); });
    engine.close();
}

TEST_F(LiveRestore, ANameIsAStrategysForGood) {
    const std::string option = model::option_symbol({.root = "SPY",
                                                         .expiry = 2099y / December / 18,
                                                         .type = model::OptionType::PUT,
                                                         .strike = 450});
    std::string second;
    {
        core::live::MainEngine engine;
        const std::string first = engine.add_strategy("StraddleTestStrategy", "SPY", {});
        EXPECT_EQ(first, "StraddleTestStrategy_SPY");
        fill(engine, first, "500", option, 1, 2.0);
        ASSERT_TRUE(eventually([&] { return quantity(engine, first, option) == 1; }));
        engine.on_event_thread( // sold back: flat
            [&] { engine.execution_engine()->register_active_order(first, "501"); });
        model::TradeData* t = engine.acquire_trade();
        *t = {};
        t->tradeid = "T501";
        t->orderid = "501";
        t->symbol = option;
        t->direction = Direction::SHORT;
        t->price = 3.0;
        t->volume = 1;
        engine.put_event(model::Event(model::EventType::Trade, t));
        ASSERT_TRUE(eventually([&] { return quantity(engine, first, option) == 0; }));

        engine.delete_strategy(first); // its fills stay, under its name
        EXPECT_THROW(engine.add_strategy("StraddleTestStrategy", "SPY", {}, first),
                     std::invalid_argument);
        second = engine.add_strategy("StraddleTestStrategy", "SPY", {});
        EXPECT_EQ(second, "StraddleTestStrategy_SPY_2");
        engine.close();
    }
    core::live::MainEngine engine; // the second does not take the first's fills
    const model::StrategyHolding* h = engine.position_engine()->find_holding(second);
    ASSERT_NE(h, nullptr);
    EXPECT_TRUE(h->optionPositions.empty());
    EXPECT_EQ(engine.option_strategy_engine()->get_strategy("StraddleTestStrategy_SPY"), nullptr);
    engine.close();
}

TEST_F(LiveRestore, TheRiskLimitsAndAHaltOutliveARestart) {
    std::string name;
    {
        core::live::MainEngine engine;
        name = engine.add_strategy("ThreadProbeStrategy", "SPY", {});
        engine.init_strategy(name);
        engine.set_risk_settings({{"max_option_order", 7}, {"short_options", 0}});
        EXPECT_THROW(engine.set_risk_settings({{"max_option_order", -1}}), std::invalid_argument);
        engine.halt("a test");
        EXPECT_THROW(engine.start_strategy(name), std::runtime_error);
        engine.close();
    }
    core::live::MainEngine engine;
    const auto [halted, reason, limit, shorts] = engine.on_event_thread([&] {
        const core::RiskEngine& r = *engine.risk_engine();
        return std::tuple(r.halted(), r.halt_reason(), r.setting("max_option_order"),
                          r.setting("short_options"));
    });
    EXPECT_TRUE(halted);
    EXPECT_EQ(reason, "a test");
    EXPECT_EQ(limit, 7);
    EXPECT_EQ(shorts, 0);
    // Why a start is refused: halted, then (the halt cleared) not that: with a broker it still
    // waits for a connect, without one it starts.
    const auto refusal = [&] {
        try {
            engine.start_strategy(name);
        } catch (const std::runtime_error& e) {
            return std::string(e.what());
        }
        return std::string();
    };
    EXPECT_NE(refusal().find("halted"), std::string::npos);
    engine.clear_halt();
    const std::string after = refusal();
    EXPECT_EQ(after.find("halted"), std::string::npos) << after;
    EXPECT_TRUE(after.empty() || after.find("Connect first") != std::string::npos) << after;
    engine.close();
}

TEST_F(LiveRestore, AFillBookedByHandIsHeldAndRebooked) {
    const std::string option = model::option_symbol({.root = "SPY",
                                                         .expiry = 2099y / December / 18,
                                                         .type = model::OptionType::CALL,
                                                         .strike = 640});
    {
        // The contract the fill is in, as a sync saves it.
        model::ContractData c;
        c.symbol = option;
        c.product = model::Product::OPTION;
        c.size = 100;
        c.trading_class = "SPY";
        c.option_strike = 640;
        c.option_type = model::OptionType::CALL;
        c.option_expiry = model::option_expiry_time(2099y / December / 18);
        c.option_underlying = "SPY";
        adapters::DatabaseEngine db(nullptr, path_);
        db.save_contracts(std::vector{c});
        db.close();
    }
    std::string name;
    {
        core::live::MainEngine engine;
        name = engine.add_strategy("StraddleTestStrategy", "SPY", {});
        EXPECT_THROW(engine.book_manually(name, option, Direction::LONG, 2, 1.25, ""),
                     std::invalid_argument); // no reason given
        EXPECT_THROW(engine.book_manually(name, "SPY-20991218-C-1", Direction::LONG, 2, 1.25,
                                          "missed"),
                     std::invalid_argument); // no such contract
        const std::string id =
            engine.book_manually(name, option, Direction::LONG, 2, 1.25, "filled while down");
        EXPECT_TRUE(id.starts_with("manual-"));
        EXPECT_EQ(quantity(engine, name, option), 2);
        engine.close();
    }
    core::live::MainEngine engine;
    EXPECT_EQ(quantity(engine, name, option), 2);
    EXPECT_DOUBLE_EQ(
        engine.position_engine()->find_holding(name)->optionPositions.at(option).avg_cost, 1.25);
    engine.close();
}

} // namespace
