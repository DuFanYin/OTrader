#include "core/bookkeeping/bookkeeping_engine.hpp"
#include "core/strategy/strategy_engine.hpp"
#include "tests/lone_backtest.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/strategy.hpp"
#include "model/event.hpp"
#include "model/symbol.hpp"
#include <gtest/gtest.h>
#include <stdexcept>

namespace {

using model::Side;
using model::Event;
using model::EventType;
using model::Status;

class DispatchContractTestStrategy final : public strategy::Strategy {
  public:
    using Strategy::Strategy;

    void on_init() override {}
    void on_stop() override {}
    void on_timer() override {}

    void on_order(const model::Order& order) override {
        // Contract: the book has the latest order before the strategy is told.
        auto* cached = execution->find_order(order.order_id);
        ASSERT_NE(cached, nullptr);
        EXPECT_EQ(cached->status, order.status);
        saw_order_ = true;
    }

    void on_fill(const model::Fill& fill) override {
        // Contract: the book has booked the fill before the strategy is told.
        ASSERT_NE(holding(), nullptr);
        if (!model::parse_option_symbol(fill.symbol)) { // the underlying
            EXPECT_EQ(holding()->shares.quantity, 1);
        }
        saw_trade_ = true;
    }

    bool saw_order() const { return saw_order_; }
    bool saw_trade() const { return saw_trade_; }

    // The engine's, to check its state from inside the callbacks.
    core::BookkeepingEngine* execution = nullptr;

  private:
    bool saw_order_ = false;
    bool saw_trade_ = false;
};

REGISTER_STRATEGY(DispatchContractTestStrategy);

TEST(BacktestDispatchContract, ExecutionAndPositionAreUpdatedBeforeStrategyCallbacks) {
    test::LoneBacktest backtest;
    core::Main& main = backtest.main();
    ASSERT_NE(main.strategy_engine(), nullptr);
    ASSERT_NE(main.bookkeeping_engine(), nullptr);

    const std::string portfolio = "P";
    ASSERT_NE(main.portfolios(), nullptr);
    main.portfolios()->ensure_portfolio(portfolio);
    main.strategy_engine()->add_strategy(
        {.class_name = "DispatchContractTestStrategy", .portfolio = portfolio});
    auto* strat = static_cast<DispatchContractTestStrategy*>(
        main.strategy_engine()->find_strategy("DispatchContractTestStrategy_" + portfolio));
    ASSERT_NE(strat, nullptr);
    strat->execution = main.bookkeeping_engine();

    const std::string oid = "OID-1";
    const std::string strategy_name = "DispatchContractTestStrategy_" + portfolio;
    main.bookkeeping_engine()->add_order(
        strategy_name,
        {.symbol = "AAPL", .order_id = oid, .legs = {{.ratio = 1, .symbol = "AAPL"}}});

    // Order event
    auto* ord = main.event_loop()->acquire_order();
    ASSERT_NE(ord, nullptr);
    ord->order_id = oid;
    ord->symbol = "AAPL";
    ord->status = Status::WORKING;
    main.event_loop()->put_event(Event(EventType::Order, ord));
    EXPECT_TRUE(strat->saw_order());

    // Trade event
    auto* tr = main.event_loop()->acquire_fill();
    ASSERT_NE(tr, nullptr);
    tr->order_id = oid;
    tr->fill_id = "TID-1";
    tr->symbol = "AAPL";
    tr->side = Side::BUY;
    tr->price = 100.0;
    tr->quantity = 1;
    main.event_loop()->put_event(Event(EventType::Fill, tr));
    EXPECT_TRUE(strat->saw_trade());
}

} // namespace
