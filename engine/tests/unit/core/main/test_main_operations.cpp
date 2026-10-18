/**
 * Main's operations over the backtest assembly, its clock the latest snapshot's: a terminal update
 * waits for its fills only so long; an option settled in shares is closed in its last minutes
 * through its strategy; a fill booked by hand and a settlement by hand check what they are given;
 * a halt stops the strategies and cancels what is working; a strategy's hedging, registered through
 * its context, hedges its delta on the timer.
 */
#include "tests/read_main.hpp"
#include "tests/lone_backtest.hpp"
#include "model/order_builder.hpp"
#include "model/event.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/strategy.hpp"
#include <array>
#include <chrono>
#include <cstddef>
#include <format>
#include <gtest/gtest.h>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using namespace std::chrono;
using model::Side;
using model::OrderType;

/** Remembers the last order update it heard of. */
class MainProbe final : public strategy::Strategy {
  public:
    using Strategy::Strategy;

    void on_timer() override {}
    void on_stop() override {
        if (close_on_stop) {
            close_all_strategy_positions();
        }
    }
    void on_order(const model::Order& order) override { last_order = order.status; }

    std::optional<model::Status> last_order;
    bool close_on_stop = false;
};

REGISTER_STRATEGY(MainProbe);

// A Friday: the call expires at its 16:00 New York close (20:00 UTC).
const year_month_day kFriday = 2025y / March / 14;

class MainOperations : public ::testing::Test {
  protected:
    void SetUp() override {
        model::Portfolios& s = *main.portfolios();
        s.ensure_portfolios_created(); // SPY's options settle in shares
        model::Contract spy;
        spy.symbol = "SPY";
        spy.product = model::Product::EQUITY;
        model::Contract call;
        call.symbol = call_;
        call.product = model::Product::OPTION;
        call.multiplier = 100;
        s.load_contracts(std::vector{spy, call});
        model::Portfolio& p = *s.find_portfolio("SPY");
        model::PortfolioSnapshot quotes = p.snapshot();
        quotes.underlying_bid = 599.99;
        quotes.underlying_ask = 600.01;
        quotes.values(&model::Frame::bid)[0] = 1.9;
        quotes.values(&model::Frame::ask)[0] = 2.1;
        quotes.priced = true;
        p.apply(quotes);
        backtest.configure_execution({.fee_rate = 0.0});
        name_ = main.add_strategy("MainProbe", "SPY", {});
        main.init_strategy(name_);
        main.start_strategy(name_);
        probe_ = static_cast<MainProbe*>(main.strategy_engine()->find_strategy(name_));
    }

    /** The engine's time, and its portfolio's, is now `t`. */
    void at(model::DateTime t) {
        model::PortfolioSnapshot snapshot(nullptr, "elsewhere"); // only the clock moves
        snapshot.datetime = t;
        main.apply_snapshot(snapshot);
        model::Portfolio& spy = *main.portfolios()->find_portfolio("SPY");
        model::PortfolioSnapshot moved = spy.snapshot();
        moved.assign(spy.frame());
        moved.datetime = t;
        spy.apply(moved);
    }
    void tick() { main.event_loop()->put_event(model::Event(model::EventType::Timer)); }
    /** The next bar: what was sent fills at its quotes. */
    void fill() { backtest.backtest_gateway()->execute_pending_orders(); }
    /** The strategy buys `quantity` of `symbol` at market. */
    auto buy(const std::string& symbol, double quantity) -> std::string {
        const std::array leg{model::contract_leg(*main.find_contract(symbol), Side::BUY)};
        return main.send_order(optra_tests::record_of(main, name_),
                               model::order_request(leg, 0.0, quantity, OrderType::MARKET));
    }
    auto holding() -> const model::StrategyHolding& {
        return *main.bookkeeping_engine()->find_holding(name_);
    }
    auto held(const std::string& symbol) -> int {
        const auto it = holding().options.find(symbol);
        return it != holding().options.end() ? it->second.quantity : 0;
    }
    auto running() -> bool {
        return main.strategy_engine()->find_record(name_)->state == model::StrategyState::Running;
    }
    auto working() -> std::vector<model::Order> {
        std::vector<model::Order> active;
        main.bookkeeping_engine()->for_each_active_order(
            {}, [&](const model::Order& o) { active.push_back(o); });
        return active;
    }

    const std::string call_ = model::option_symbol(
        {.root = "SPY", .expiry = kFriday, .type = model::OptionType::CALL, .strike = 600});
    const model::DateTime close_ = model::option_expiry_time(kFriday);
    test::LoneBacktest backtest;
    core::Main& main = backtest.main();
    std::string name_;
    MainProbe* probe_ = nullptr;
};

TEST_F(MainOperations, ATerminalUpdateWithoutItsFillsIsAppliedAfterTheTimeout) {
    const model::DateTime t0 = close_ - 2h;
    at(t0);
    const std::string id =
        buy(call_, 1);
    ASSERT_FALSE(id.empty());
    model::Order done = *main.bookkeeping_engine()->find_order(id);
    done.status = model::Status::FILLED; // its fill never comes
    done.filled = 1;
    main.event_loop()->put_order(done);
    EXPECT_TRUE(main.bookkeeping_engine()->is_active(id)); // held back for its fill
    at(t0 + core::BookkeepingEngine::kTerminalUpdateTimeout - 1s);
    tick();
    EXPECT_TRUE(main.bookkeeping_engine()->is_active(id));
    EXPECT_FALSE(probe_->last_order);

    at(t0 + core::BookkeepingEngine::kTerminalUpdateTimeout);
    tick();
    EXPECT_FALSE(main.bookkeeping_engine()->is_active(id));
    EXPECT_EQ(main.bookkeeping_engine()->find_order(id)->status, model::Status::FILLED);
    EXPECT_EQ(probe_->last_order, model::Status::FILLED);
    EXPECT_EQ(held(call_), 0); // only a fill is held
}

TEST_F(MainOperations, AnOptionSettledInSharesIsClosedInItsLastMinutes) {
    main.set_risk_settings({{"flatten_minutes", 10}});
    at(close_ - 30min);
    buy(call_, 2);
    fill();
    ASSERT_EQ(held(call_), 2);
    tick();
    EXPECT_TRUE(working().empty()); // not yet

    at(close_ - 5min);
    tick();
    ASSERT_EQ(working().size(), 1U);
    EXPECT_EQ(working()[0].symbol, call_);
    EXPECT_EQ(working()[0].legs.front().side, Side::SELL);
    EXPECT_EQ(working()[0].quantity, 2.0);
    at(close_ - 4min);
    tick();
    EXPECT_EQ(working().size(), 1U); // the close still working: none stacked on it

    fill();
    EXPECT_EQ(held(call_), 0);
    EXPECT_TRUE(holding().is_flat()); // no shares will be delivered
}

TEST_F(MainOperations, AFillBookedByHandIsCheckedAndBookedOnce) {
    at(close_ - 2h);
    const auto book = [&](const std::string& strategy, const std::string& symbol, double quantity,
                          double price, const std::string& reason) {
        return main.book_manually(strategy, symbol, Side::BUY, quantity, price, reason);
    };
    EXPECT_THROW(book("NoSuchStrategy", call_, 1, 2.0, "missed"), std::invalid_argument);
    EXPECT_THROW(book(name_, "SPY-20250314-C-999", 1, 2.0, "missed"), std::invalid_argument);
    EXPECT_THROW(book(name_, call_, 0, 2.0, "missed"), std::invalid_argument);
    EXPECT_THROW(book(name_, call_, 1, -1.0, "missed"), std::invalid_argument);
    EXPECT_THROW(book(name_, call_, 1, 2.0, ""), std::invalid_argument);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(book(name_, call_, nan, 2.0, "missed"), std::invalid_argument);
    EXPECT_THROW(book(name_, call_, 1, nan, "missed"), std::invalid_argument);
    EXPECT_EQ(held(call_), 0);

    const auto ms = duration_cast<milliseconds>((close_ - 2h).time_since_epoch()).count();
    EXPECT_EQ(book(name_, call_, 1, 2.0, "missed"),
              std::format("manual-{}-{}-{}", name_, call_, ms)); // by strategy, contract, time
    // The same at the same time has the same id: refused, not booked twice.
    EXPECT_THROW(book(name_, call_, 1, 2.0, "missed"), std::runtime_error);
    EXPECT_EQ(held(call_), 1);
    at(close_ - 2h + 1s);
    book(name_, call_, 1, 2.0, "missed again");
    EXPECT_EQ(held(call_), 2);

    const auto holdings = optra_tests::holdings(main);
    ASSERT_EQ(holdings.size(), 1U);
    EXPECT_EQ(holdings.at(name_).options.at(call_).quantity, 2);
}

TEST_F(MainOperations, ASettlementByHandChecksItsExpiryAndPrice) {
    at(close_ - 1h);
    main.book_manually(name_, call_, Side::BUY, 1, 4.0, "held");
    EXPECT_THROW(main.settle_expired("NOPE", kFriday, 610.0), std::invalid_argument);
    EXPECT_THROW(main.settle_expired("SPY", 2025y / February / 30, 610.0), std::invalid_argument);
    EXPECT_THROW(main.settle_expired("SPY", kFriday, 0.0), std::invalid_argument);
    EXPECT_THROW(main.settle_expired("SPY", kFriday, 610.0), std::invalid_argument); // not closed
    EXPECT_EQ(held(call_), 1);

    at(close_ + 1h); // no timer: settled by hand
    EXPECT_EQ(main.settle_expired("SPY", kFriday, 610.0), 2); // the call; its shares
    EXPECT_EQ(held(call_), 0);
    EXPECT_EQ(holding().shares.quantity, 100);
    EXPECT_DOUBLE_EQ(holding().shares.avg_cost, 600.0);
}

TEST_F(MainOperations, AHaltStopsTheStrategiesAndCancelsWhatIsWorking) {
    at(close_ - 2h);
    buy(call_, 1);
    ASSERT_EQ(working().size(), 1U);

    main.halt("");
    EXPECT_TRUE(main.risk_engine()->halted());
    EXPECT_EQ(main.risk_engine()->halt_reason(), "by the operator");
    EXPECT_FALSE(running());
    EXPECT_EQ(optra_tests::strategies(main).front().state, model::StrategyState::Inited);
    EXPECT_TRUE(working().empty());
    fill();
    EXPECT_EQ(held(call_), 0); // the venue has it no more
    EXPECT_THROW(main.start_strategy(name_), std::runtime_error);
}

TEST_F(MainOperations, WhatAStrategyClosesAsAHaltStopsItGoes) {
    at(close_ - 2h);
    buy(call_, 1);
    fill();
    ASSERT_EQ(held(call_), 1);
    buy(call_, 1); // working as the halt comes
    probe_->close_on_stop = true;

    main.halt("");
    const std::vector<model::Order> left = working();
    ASSERT_EQ(left.size(), 1U) << "the buy cancelled, the close sent as it stopped kept";
    EXPECT_EQ(left.front().legs.front().side, Side::SELL);
    fill();
    EXPECT_EQ(held(call_), 0);
}

TEST_F(MainOperations, HedgingRegisteredThroughTheContextHedgesOnTheTimer) {
    probe_->register_hedging(1, 0, 5);
    ASSERT_NE(main.strategy_engine()->hedging(name_), nullptr);
    EXPECT_EQ(main.strategy_engine()->hedging(name_)->delta_range, 5);
    at(close_ - 2h);
    main.book_manually(name_, "SPY", Side::BUY, 10, 600.0, "shares"); // delta 10
    tick();
    ASSERT_EQ(working().size(), 1U);
    EXPECT_EQ(working()[0].symbol, "SPY");
    EXPECT_EQ(working()[0].legs.front().side, Side::SELL);
    EXPECT_EQ(working()[0].quantity, 10.0);
    EXPECT_EQ(working()[0].reference, "Hedge_" + name_);

    probe_->unregister_hedging();
    EXPECT_EQ(main.strategy_engine()->hedging(name_), nullptr);
}

TEST_F(MainOperations, AnOrderNotSentIsNotKept) {
    at(close_ - 2h);
    main.set_risk_settings({{"max_option_order", 1}});
    const std::size_t orders = main.bookkeeping_engine()->order_count();
    EXPECT_TRUE(buy(call_, 2).empty());
    EXPECT_EQ(main.risk_engine()->refused(), 1U);
    EXPECT_EQ(main.bookkeeping_engine()->order_count(), orders);
    fill();
    EXPECT_EQ(held(call_), 0);
}

TEST_F(MainOperations, AHedgeStillWorkingAtTheNextLookIsCancelled) {
    probe_->register_hedging(1, 0, 5);
    at(close_ - 2h);
    main.book_manually(name_, "SPY", Side::BUY, 10, 600.0, "shares");
    tick();
    ASSERT_EQ(working().size(), 1U);
    at(close_ - 2h + 1min);
    tick();
    EXPECT_TRUE(working().empty());
    fill();
    EXPECT_EQ(holding().shares.quantity, 10); // the hedge never filled
}

TEST_F(MainOperations, AStrategyHoldingAnExpiredOptionNotSettledDoesNotStart) {
    at(close_ - 1h);
    main.book_manually(name_, call_, Side::BUY, 1, 4.0, "held");
    main.stop_strategy(name_);
    at(close_ + 1h); // no timer: nothing settled
    EXPECT_THROW(main.start_strategy(name_), std::runtime_error);
    EXPECT_FALSE(running());
    EXPECT_EQ(main.settle_expired("SPY", kFriday, 590.0), 1); // out of the money: no shares
    main.start_strategy(name_);
    EXPECT_TRUE(running());
}

TEST_F(MainOperations, AStrategyIsRemovedRestoredAndDeletedByName) {
    EXPECT_THROW(main.remove_strategy("NoSuchStrategy"), std::invalid_argument);
    EXPECT_THROW(main.restore_strategy(name_), std::invalid_argument); // not removed

    main.remove_strategy(name_);
    EXPECT_EQ(main.strategy_engine()->find_strategy(name_), nullptr);
    main.restore_strategy(name_);
    ASSERT_NE(main.strategy_engine()->find_strategy(name_), nullptr);
    EXPECT_EQ(optra_tests::strategies(main).front().state, model::StrategyState::Added);

    main.delete_strategy(name_);
    EXPECT_EQ(main.strategy_engine()->find_strategy(name_), nullptr);
    EXPECT_THROW(main.restore_strategy(name_), std::invalid_argument); // deleted, not removed
    EXPECT_NO_THROW(main.delete_strategy("NoSuchStrategy"));
}

TEST_F(MainOperations, AHaltKeepsItsReasonAndClearingNoHaltChangesNothing) {
    main.clear_halt();
    EXPECT_FALSE(main.risk_engine()->halted());
    main.halt("a drill");
    EXPECT_EQ(main.risk_engine()->halt_reason(), "a drill");
    main.clear_halt();
    EXPECT_FALSE(main.risk_engine()->halted());
    main.start_strategy(name_);
    EXPECT_TRUE(running());
}

TEST_F(MainOperations, ConnectingWithoutABrokerChecksNothing) {
    main.connect();
    EXPECT_EQ(main.bookkeeping_engine()->broker_check().first, "not yet");
}

TEST_F(MainOperations, AnOptionNoLongerListedCanStillBeClosedByHand) {
    at(close_ - 2h);
    main.book_manually(name_, call_, Side::BUY, 1, 2.0, "held");
    model::Contract spy;
    spy.symbol = "SPY";
    spy.product = model::Product::EQUITY;
    main.portfolios()->replace_contracts("SPY", std::vector{spy},
                                         std::unordered_set<std::string>{}); // not kept
    ASSERT_EQ(main.find_contract(call_), nullptr);
    at(close_ - 2h + 1s);
    main.book_manually(name_, call_, Side::SELL, 1, 2.5, "closed by hand");
    EXPECT_EQ(held(call_), 0);
    at(close_ - 2h + 2s);
    EXPECT_THROW(main.book_manually(name_, call_, Side::SELL, 1, 2.5, "again"),
                 std::invalid_argument); // neither listed nor held
}

TEST_F(MainOperations, TradingRecordsAreWipedOnlyWhenNothingIsHeldOrWorking) {
    EXPECT_THROW(main.wipe_database("everything"), std::invalid_argument);
    EXPECT_NO_THROW(main.wipe_database("contracts"));
    at(close_ - 2h);
    main.book_manually(name_, call_, Side::BUY, 1, 2.0, "held");
    EXPECT_THROW(main.wipe_database("trading"), std::runtime_error);
    at(close_ - 2h + 1s);
    main.book_manually(name_, call_, Side::SELL, 1, 2.0, "closed");
    buy(call_, 1);
    ASSERT_EQ(working().size(), 1U);
    EXPECT_THROW(main.wipe_database("trading"), std::runtime_error);
    main.cancel_order(working()[0].create_cancel_request());
    EXPECT_NO_THROW(main.wipe_database("trading"));
}

TEST_F(MainOperations, AnUpdateOfAnOrderNoStrategySentIsKeptAndHeardByNone) {
    model::Order unknown;
    unknown.order_id = "TWS-1";
    unknown.symbol = call_;
    unknown.status = model::Status::WORKING;
    main.event_loop()->put_order(unknown);
    ASSERT_NE(main.bookkeeping_engine()->find_order("TWS-1"), nullptr);
    EXPECT_FALSE(probe_->last_order);
}

} // namespace
