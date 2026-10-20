/**
 * The live engine starts where it stopped: a strategy comes back from the database with every
 * order and fill saved booked again (one IB reports again is dropped), and cannot be removed while
 * it holds anything. An
 * option held past its close with no closing price to settle at keeps its strategy from starting
 * until it is settled by hand; the settlement (the option closed, its shares delivered) is booked
 * and saved like any fill, so the next start rebooks it too. A name, which its fills are kept under, is a
 * strategy's for good. The record of orders and fills is emptied only when nothing is held.
 * The risk limits and a halt outlive a restart, and no strategy starts while halted; a fill booked
 * by hand is held and rebooked like any; a fill that came without a time keeps the engine's. The
 * strategies running when the engine stopped start again, each that may; a saved strategy that
 * cannot be made again is skipped.
 */
#include "tests/read_main.hpp"
#include "adapters/database/sqlite_database.hpp"
#include "assembly/live.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/strategy.hpp"
#include "model/session_calendar.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include <chrono>
#include <sqlite3.h>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono;
using model::Side;

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

/** A fill of `quantity` `symbol` for the strategy, as the gateway reports it. */
void fill(core::Main& engine, const std::string& strategy, const std::string& id,
          const std::string& symbol, double quantity, double price,
          Side side = Side::BUY) {
    model::Order order;
    order.order_id = id;
    order.symbol = symbol;
    order.legs = {{.ratio = 1, .side = side, .symbol = symbol}};
    order.type = model::OrderType::LIMIT;
    order.price = price;
    order.quantity = quantity;
    // Sent first, as Main stores it; the trading state is the event thread's.
    engine.event_loop()->on_event_thread(
        [&] { engine.bookkeeping_engine()->add_order(strategy, order); });
    order.filled = quantity;
    order.status = model::Status::FILLED;
    model::Fill fill;
    fill.fill_id = "T" + id;
    fill.order_id = id;
    fill.symbol = symbol;
    fill.side = side;
    fill.price = price;
    fill.quantity = quantity;
    model::Order* o = engine.event_loop()->acquire_order();
    *o = order;
    engine.event_loop()->put_event(model::Event(model::EventType::Order, o));
    model::Fill* t = engine.event_loop()->acquire_fill();
    *t = fill;
    engine.event_loop()->put_event(model::Event(model::EventType::Fill, t));
}

/** Notes the thread each call from the engine comes on. */
class ThreadProbeStrategy final : public strategy::Strategy {
  public:
    using Strategy::Strategy;

    void on_init() override { seen_.push_back(std::this_thread::get_id()); }
    void on_stop() override {}
    void on_timer() override { seen_.push_back(std::this_thread::get_id()); }
    void on_order(const model::Order& order) override {
        seen_.push_back(std::this_thread::get_id());
        Strategy::on_order(order);
    }
    void on_fill(const model::Fill& fill) override {
        seen_.push_back(std::this_thread::get_id());
        Strategy::on_fill(fill);
    }

    const std::vector<std::thread::id>& seen() const { return seen_; }

  private:
    std::vector<std::thread::id> seen_;
};

REGISTER_STRATEGY(ThreadProbeStrategy);

/** Counts the calls of on_stop, in every instance. */
class StopCountStrategy final : public strategy::Strategy {
  public:
    using Strategy::Strategy;

    void on_init() override {}
    void on_stop() override { ++stops; }
    void on_timer() override {}

    static inline int stops = 0;
};

REGISTER_STRATEGY(StopCountStrategy);

auto removed_strategies(core::Main& engine) -> std::vector<std::string> {
    std::vector<std::string> out;
    for (const model::StrategyRecord& r : engine.database()->load_strategies()) {
        if (r.state == model::StrategyState::Removed) {
            out.push_back(r.name);
        }
    }
    return out;
}

/** A venue that is no broker: positions are not checked against one. */
auto no_broker() -> std::unique_ptr<core::Gateway> {
    return std::make_unique<core::Gateway>(nullptr);
}

auto quantity(core::Main& engine, const std::string& strategy, const std::string& option)
    -> int {
    return engine.event_loop()->on_event_thread([&] {
        const model::StrategyHolding* h = engine.bookkeeping_engine()->find_holding(strategy);
        if (h == nullptr) {
            return -1;
        }
        const auto it = h->options.find(option);
        return it != h->options.end() ? it->second.quantity : 0;
    });
}

TEST_F(LiveRestore, AStrategyComesBackWithItsHolding) {
    const std::string option = model::option_symbol({.root = "SPY",
                                                         .expiry = 2099y / December / 18,
                                                         .type = model::OptionType::CALL,
                                                         .strike = 600});
    std::string name;
    {
        const auto made = live::make_main();
        core::Main& engine = *made;
        name = engine.add_strategy("RandomBuyStrategy", "SPY", {});
        fill(engine, name, "100", option, 2, 1.5);
        ASSERT_TRUE(eventually([&] { return quantity(engine, name, option) == 2; }));
        engine.close();
    }
    const auto made = live::make_main();
    core::Main& engine = *made;
    ASSERT_NE(engine.strategy_engine()->find_strategy(name), nullptr);
    EXPECT_EQ(quantity(engine, name, option), 2);
    EXPECT_DOUBLE_EQ(
        engine.bookkeeping_engine()->find_holding(name)->options.at(option).avg_cost, 1.5);
    EXPECT_EQ(engine.bookkeeping_engine()->strategy_of("100"), name);
    EXPECT_THROW(engine.remove_strategy(name), std::runtime_error); // it holds a call
    EXPECT_TRUE(removed_strategies(engine).empty());
    engine.close();
}

TEST_F(LiveRestore, AFillThatCameWithoutATimeIsStampedWithTheEnginesAndKeptSo) {
    const std::string option = model::option_symbol({.root = "SPY",
                                                         .expiry = 2099y / December / 18,
                                                         .type = model::OptionType::CALL,
                                                         .strike = 600});
    const model::DateTime before = floor<seconds>(system_clock::now());
    std::string name;
    {
        const auto made = live::make_main();
        core::Main& engine = *made;
        name = engine.add_strategy("RandomBuyStrategy", "SPY", {});
        fill(engine, name, "110", option, 1, 1.5); // no time, as IB's
        ASSERT_TRUE(eventually([&] { return quantity(engine, name, option) == 1; }));
        engine.close();
    }
    const auto made = live::make_main();
    core::Main& engine = *made;
    const model::DateTime last = engine.event_loop()->on_event_thread(
        [&] { return engine.bookkeeping_engine()->find_holding(name)->last_fill; });
    EXPECT_GE(last, before); // what traded_today() reads, after a restart too
    EXPECT_LE(last, system_clock::now());
    engine.close();
}

TEST_F(LiveRestore, ClosingTheEngineDoesNotStopItsStrategies) {
    // on_stop may close the strategy's positions; a restart resumes them instead.
    StopCountStrategy::stops = 0;
    std::string name;
    {
        const auto made = live::make_main(no_broker());
        core::Main& engine = *made;
        name = engine.add_strategy("StopCountStrategy", "SPY", {});
        engine.init_strategy(name);
        engine.start_strategy(name);
        engine.close();
    }
    EXPECT_EQ(StopCountStrategy::stops, 0);
    const auto made = live::make_main(no_broker());
    core::Main& engine = *made;
    const std::vector<model::StrategyRecord> saved = optra_tests::strategies(engine);
    ASSERT_EQ(saved.size(), 1U);
    EXPECT_EQ(saved.front().state, model::StrategyState::Inited) << "back inited, until resumed";
    EXPECT_THROW(engine.stop_strategy(name), std::logic_error) << "not running: no stop";
    EXPECT_EQ(StopCountStrategy::stops, 0);
    engine.start_strategy(name);
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
        const auto made = live::make_main();
        core::Main& engine = *made;
        name = engine.add_strategy("RandomBuyStrategy", "SPY", {});
        fill(engine, name, "300", option, 3, 2.0);
        fill(engine, name, "301", option, 1, 1.0, Side::SELL);
        ASSERT_TRUE(eventually([&] { return quantity(engine, name, option) == 2; }));
        engine.close();
    }
    const auto made = live::make_main();
    core::Main& engine = *made;
    EXPECT_EQ(quantity(engine, name, option), 2);
    const auto position = [&] {
        return engine.event_loop()->on_event_thread([&] {
            return engine.bookkeeping_engine()->find_holding(name)->options.at(option);
        });
    };
    EXPECT_DOUBLE_EQ(position().avg_cost, 2.0);
    EXPECT_DOUBLE_EQ(position().realized_pnl, -100.0); // one sold at 1.0, bought at 2.0
    // IB repeats today's fills on a connect: the one booked already is dropped.
    fill(engine, name, "300", option, 3, 2.0);
    EXPECT_TRUE(eventually([&] { return engine.event_loop()->on_event_thread([&] {
        return engine.bookkeeping_engine()->find_fill("T300") != nullptr; }); }));
    EXPECT_EQ(quantity(engine, name, option), 2);
    engine.close();
}

TEST_F(LiveRestore, TheTradingRecordIsEmptiedOnlyWhenNothingIsHeld) {
    const std::string option = model::option_symbol({.root = "SPY",
                                                         .expiry = 2099y / December / 18,
                                                         .type = model::OptionType::PUT,
                                                         .strike = 630});
    const auto made = live::make_main();
    core::Main& engine = *made;
    const std::string name = engine.add_strategy("RandomBuyStrategy", "SPY", {});
    fill(engine, name, "320", option, 1, 2.0);
    ASSERT_TRUE(eventually([&] { return quantity(engine, name, option) == 1; }));
    EXPECT_THROW(engine.wipe_database("trading"), std::runtime_error);
    EXPECT_THROW(engine.wipe_database("everything"), std::invalid_argument);
    EXPECT_EQ(engine.database()->strategies_with_records(), std::vector<std::string>{name});

    fill(engine, name, "321", option, 1, 2.5, Side::SELL); // flat
    ASSERT_TRUE(eventually([&] { return quantity(engine, name, option) == 0; }));
    const std::vector<std::pair<std::string, int>> emptied = engine.wipe_database("trading");
    EXPECT_EQ(emptied, (std::vector<std::pair<std::string, int>>{{"orders", 2}, {"fills", 2}}));
    EXPECT_TRUE(engine.database()->load_orders({}, 0).empty());
    EXPECT_TRUE(engine.database()->strategies_with_records().empty());
    engine.close();
}

TEST_F(LiveRestore, ARemovedStrategyIsRestoredWithItsFills) {
    const std::string option = model::option_symbol({.root = "SPY",
                                                         .expiry = 2099y / December / 18,
                                                         .type = model::OptionType::PUT,
                                                         .strike = 500});
    const auto made = live::make_main();
    core::Main& engine = *made;
    const std::string name = engine.add_strategy("RandomBuyStrategy", "SPY", {}, "kept");
    fill(engine, name, "200", option, 1, 2.0);
    ASSERT_TRUE(eventually([&] { return quantity(engine, name, option) == 1; }));
    fill(engine, name, "201", option, 1, 3.0, Side::SELL); // sold at 3: flat, 100 made
    ASSERT_TRUE(eventually([&] { return quantity(engine, name, option) == 0; }));

    engine.remove_strategy(name);
    EXPECT_EQ(engine.strategy_engine()->find_strategy(name), nullptr);
    EXPECT_EQ(removed_strategies(engine), std::vector<std::string>{name});
    engine.restore_strategy(name);
    ASSERT_NE(engine.strategy_engine()->find_strategy(name), nullptr);
    EXPECT_DOUBLE_EQ(
        engine.bookkeeping_engine()->find_holding(name)->closed_options.at(option).realized_pnl,
        100.0);
    EXPECT_TRUE(removed_strategies(engine).empty());
    engine.close();
}

TEST_F(LiveRestore, AnExpiredOptionWaitsForItsCloseThenSettlesInShares) {
    const auto expiry = year_month_day{floor<days>(system_clock::now()) - days(3)};
    const std::string call = model::option_symbol(
        {.root = "SPY", .expiry = expiry, .type = model::OptionType::CALL, .strike = 580});
    std::string name;
    {
        const auto made = live::make_main();
        core::Main& engine = *made;
        name = engine.add_strategy("RandomBuyStrategy", "SPY", {});
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
            const auto* h = engine.bookkeeping_engine()->find_holding(name);
            return quantity(engine, name, call) == 0 && h->shares.quantity == 100;
        }));
        EXPECT_TRUE(engine.event_loop()->on_event_thread(
            [&] { return engine.bookkeeping_engine()->unsettled_options().empty(); }))
            << "nothing waits once settled, before any quote";
        const auto* h = engine.bookkeeping_engine()->find_holding(name);
        EXPECT_EQ(h->shares.symbol, "SPY");
        EXPECT_DOUBLE_EQ(h->shares.avg_cost, 580.0);            // bought at the strike
        EXPECT_DOUBLE_EQ(h->closed_options.at(call).realized_pnl, -400.0); // the premium
        EXPECT_THROW(engine.settle_expired("DIA", expiry, 585.0), std::invalid_argument);
        engine.close();
    }
    const auto made = live::make_main();
    core::Main& engine = *made; // the settlement was saved like any fill
    const auto* h = engine.bookkeeping_engine()->find_holding(name);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(quantity(engine, name, call), 0);
    EXPECT_EQ(h->shares.quantity, 100);
    engine.close();
}

TEST_F(LiveRestore, AnExpiredComboSettlesContractByContract) {
    const auto expiry = year_month_day{floor<days>(system_clock::now()) - days(3)};
    const auto option = [&](model::OptionType type) {
        return model::option_symbol({.root = "SPY", .expiry = expiry, .type = type, .strike = 580});
    };
    const std::string call = option(model::OptionType::CALL);
    const std::string put = option(model::OptionType::PUT);
    const std::string combo = call + "|" + put;
    const auto made = live::make_main();
    core::Main& engine = *made;
    const std::string name = engine.add_strategy("RandomBuyStrategy", "SPY", {});
    model::Order sent; // a straddle bought, as Main::send_order stores it
    sent.order_id = "400";
    sent.symbol = "straddle_" + combo;
    sent.quantity = 1;
    sent.combo_type = model::ComboType::STRADDLE;
    for (const std::string& leg : {call, put}) {
        sent.legs.push_back({.ratio = 1, .side = Side::BUY, .symbol = leg});
    }
    engine.event_loop()->on_event_thread(
        [&] { engine.bookkeeping_engine()->add_order(name, sent); });
    for (const std::string& leg : {call, put}) {
        model::Fill* t = engine.event_loop()->acquire_fill();
        *t = {.symbol = leg,
              .order_id = "400",
              .fill_id = "T400" + leg,
              .price = 2.0,
              .quantity = 1};
        engine.event_loop()->put_event(model::Event(model::EventType::Fill, t));
    }
    ASSERT_TRUE(eventually([&] { return quantity(engine, name, put) == 1; }));
    EXPECT_EQ(quantity(engine, name, call), 1);

    EXPECT_EQ(engine.settle_expired("SPY", expiry, 585.0), 3); // the call, the put, the shares
    EXPECT_EQ(quantity(engine, name, call), 0);
    EXPECT_EQ(quantity(engine, name, put), 0);
    engine.event_loop()->on_event_thread([&] {
        const model::StrategyHolding* h = engine.bookkeeping_engine()->find_holding(name);
        ASSERT_NE(h, nullptr);
        EXPECT_TRUE(h->options.empty());
        EXPECT_EQ(h->closed_options.size(), 2U);
        EXPECT_EQ(h->closed_options.at(call).combo, combo)
            << "still says what it was opened in";
        EXPECT_EQ(h->shares.quantity, 100);
    });
    engine.close();
}

TEST_F(LiveRestore, StrategiesRunOnTheEventThread) {
    const std::string option = model::option_symbol({.root = "SPY",
                                                         .expiry = 2099y / December / 18,
                                                         .type = model::OptionType::CALL,
                                                         .strike = 620});
    const auto made = live::make_main();
    core::Main& engine = *made;
    const std::string name = engine.add_strategy("ThreadProbeStrategy", "SPY", {});
    engine.init_strategy(name);
    fill(engine, name, "600", option, 1, 1.0);
    // Queued behind the fill's events: they have reached the strategy by then.
    const auto [event_thread, seen] = engine.event_loop()->on_event_thread([&] {
        const auto* probe = dynamic_cast<const ThreadProbeStrategy*>(
            engine.strategy_engine()->find_strategy(name));
        return std::pair{std::this_thread::get_id(), probe->seen()};
    });
    EXPECT_NE(event_thread, std::this_thread::get_id());
    ASSERT_EQ(seen.size(), 3U); // on_init, on_order, on_fill
    for (const std::thread::id id : seen) {
        EXPECT_EQ(id, event_thread);
    }
    engine.close();
}

TEST_F(LiveRestore, ACancelGoesToTheGateway) {
    // It once came back to the engine, round and round until the stack ran out.
    const auto made = live::make_main();
    core::Main& engine = *made;
    model::CancelRequest req;
    req.order_id = "1";
    req.symbol = "SPY";
    engine.event_loop()->on_event_thread([&] { engine.cancel_order(req); });
    engine.close();
}

TEST_F(LiveRestore, ANameIsAStrategysForGood) {
    const std::string option = model::option_symbol({.root = "SPY",
                                                         .expiry = 2099y / December / 18,
                                                         .type = model::OptionType::PUT,
                                                         .strike = 450});
    std::string second;
    {
        const auto made = live::make_main();
        core::Main& engine = *made;
        const std::string first = engine.add_strategy("RandomBuyStrategy", "SPY", {});
        EXPECT_EQ(first, "RandomBuyStrategy_SPY");
        fill(engine, first, "500", option, 1, 2.0);
        ASSERT_TRUE(eventually([&] { return quantity(engine, first, option) == 1; }));
        engine.event_loop()->on_event_thread([&] { // sold back: flat
            engine.bookkeeping_engine()->add_order(
                first, {.symbol = option,
                        .order_id = "501",
                        .quantity = 1,
                        .filled = 1,
                        .status = model::Status::FILLED,
                        .legs = {{.ratio = 1, .side = Side::SELL, .symbol = option}}});
        });
        model::Fill* t = engine.event_loop()->acquire_fill();
        *t = {};
        t->fill_id = "T501";
        t->order_id = "501";
        t->symbol = option;
        t->side = Side::SELL;
        t->price = 3.0;
        t->quantity = 1;
        engine.event_loop()->put_event(model::Event(model::EventType::Fill, t));
        ASSERT_TRUE(eventually([&] { return quantity(engine, first, option) == 0; }));

        engine.delete_strategy(first); // its fills stay, under its name
        EXPECT_THROW(engine.add_strategy("RandomBuyStrategy", "SPY", {}, first),
                     std::invalid_argument);
        second = engine.add_strategy("RandomBuyStrategy", "SPY", {});
        EXPECT_EQ(second, "RandomBuyStrategy_SPY_2");
        engine.close();
    }
    const auto made = live::make_main();
    core::Main& engine = *made; // the second does not take the first's fills
    const model::StrategyHolding* h = engine.bookkeeping_engine()->find_holding(second);
    ASSERT_NE(h, nullptr);
    EXPECT_TRUE(h->options.empty());
    EXPECT_EQ(engine.strategy_engine()->find_strategy("RandomBuyStrategy_SPY"), nullptr);
    engine.close();
}

TEST_F(LiveRestore, TheRiskLimitsAndAHaltOutliveARestart) {
    std::string name;
    {
        const auto made = live::make_main();
        core::Main& engine = *made;
        name = engine.add_strategy("ThreadProbeStrategy", "SPY", {});
        engine.init_strategy(name);
        engine.set_risk_settings({{"max_option_order", 7}, {"short_options", 0}});
        EXPECT_THROW(engine.set_risk_settings({{"max_option_order", -1}}), std::invalid_argument);
        engine.halt("a test");
        EXPECT_THROW(engine.start_strategy(name), std::runtime_error);
        engine.close();
    }
    const auto made = live::make_main();
    core::Main& engine = *made;
    const auto [halted, reason, limit, shorts] = engine.event_loop()->on_event_thread([&] {
        const core::RiskEngine& r = *engine.risk_engine();
        return std::tuple(r.halted(), r.halt_reason(), r.limit("max_option_order"),
                          r.limit("short_options"));
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
        model::Contract c;
        c.symbol = option;
        c.product = model::Product::OPTION;
        c.multiplier = 100;
        c.trading_class = "SPY";
        c.option_strike = 640;
        c.option_type = model::OptionType::CALL;
        c.option_expiry = model::kNyse.expiry_time(2099y / December / 18);
        c.option_underlying = "SPY";
        adapters::SqliteDatabase db(nullptr, path_);
        db.save_contracts(std::vector{c});
        db.close();
    }
    std::string name;
    {
        const auto made = live::make_main();
        core::Main& engine = *made;
        name = engine.add_strategy("RandomBuyStrategy", "SPY", {});
        EXPECT_THROW(engine.book_manually(name, option, Side::BUY, 2, 1.25, ""),
                     std::invalid_argument); // no reason given
        EXPECT_THROW(engine.book_manually(name, "SPY-20991218-C-1", Side::BUY, 2, 1.25,
                                          "missed"),
                     std::invalid_argument); // no such contract
        const std::string id =
            engine.book_manually(name, option, Side::BUY, 2, 1.25, "filled while down");
        EXPECT_TRUE(id.starts_with("manual-"));
        EXPECT_EQ(quantity(engine, name, option), 2);
        engine.close();
    }
    const auto made = live::make_main();
    core::Main& engine = *made;
    EXPECT_EQ(quantity(engine, name, option), 2);
    EXPECT_DOUBLE_EQ(
        engine.bookkeeping_engine()->find_holding(name)->options.at(option).avg_cost, 1.25);
    engine.close();
}

auto states(core::Main& engine) -> std::map<std::string, model::StrategyState> {
    std::map<std::string, model::StrategyState> out;
    for (const model::StrategyRecord& r : optra_tests::strategies(engine)) {
        out[r.name] = r.state;
    }
    return out;
}

TEST_F(LiveRestore, TheStrategiesRunningWhenTheEngineStoppedStartAgainUnlessRefused) {
    const auto expiry = year_month_day{floor<days>(system_clock::now()) - days(3)};
    const std::string expired = model::option_symbol(
        {.root = "SPY", .expiry = expiry, .type = model::OptionType::CALL, .strike = 580});
    std::string runs;
    std::string blocked;
    std::string idle;
    {
        const auto made = live::make_main(no_broker());
        core::Main& engine = *made;
        runs = engine.add_strategy("StopCountStrategy", "SPY", {});
        blocked = engine.add_strategy("StopCountStrategy", "SPY", {});
        idle = engine.add_strategy("StopCountStrategy", "SPY", {});
        for (const std::string& name : {runs, blocked, idle}) {
            engine.init_strategy(name);
        }
        engine.start_strategy(runs);
        engine.start_strategy(blocked);
        // An option past its close and not settled: a start is refused until it is.
        fill(engine, blocked, "700", expired, 1, 1.0);
        ASSERT_TRUE(eventually([&] { return quantity(engine, blocked, expired) == 1; }));
        engine.close();
    }
    const auto made = live::make_main(no_broker());
    core::Main& engine = *made;
    EXPECT_EQ(states(engine).at(runs), model::StrategyState::Inited); // restored, not started
    engine.start_strategies_that_were_running();                     // a refusal is logged
    const auto after = states(engine);
    EXPECT_EQ(after.at(runs), model::StrategyState::Running);
    EXPECT_EQ(after.at(blocked), model::StrategyState::Inited);
    EXPECT_EQ(after.at(idle), model::StrategyState::Inited); // not running when it stopped
    engine.close();
}

TEST_F(LiveRestore, ASavedStrategyThatCannotBeMadeAgainIsSkipped) {
    std::string kept;
    {
        const auto made = live::make_main();
        core::Main& engine = *made;
        kept = engine.add_strategy("StopCountStrategy", "SPY", {});
        engine.close();
    }
    {
        adapters::SqliteDatabase db(nullptr, path_);
        db.save_strategy({.name = "Retired_SPY",
                          .class_name = "RetiredStrategy", // a class no library has now
                          .portfolio = "SPY",
                          .state = model::StrategyState::Inited});
        db.close();
    }
    const auto made = live::make_main();
    core::Main& engine = *made;
    EXPECT_NE(engine.strategy_engine()->find_strategy(kept), nullptr);
    EXPECT_EQ(engine.strategy_engine()->find_strategy("Retired_SPY"), nullptr);
    EXPECT_EQ(optra_tests::strategies(engine).size(), 1U);
    engine.close();
}

} // namespace
