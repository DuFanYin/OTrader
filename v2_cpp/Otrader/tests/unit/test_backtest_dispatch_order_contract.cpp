#include "runtime/backtest/engine_main.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/template.hpp"
#include "utilities/event.hpp"
#include <gtest/gtest.h>
#include <stdexcept>

namespace {

using utilities::Direction;
using utilities::Event;
using utilities::EventType;
using utilities::Status;

class DispatchContractTestStrategy final : public strategy_cpp::OptionStrategyTemplate {
  public:
    using OptionStrategyTemplate::OptionStrategyTemplate;

    void on_init_logic() override {}
    void on_stop_logic() override {}
    void on_timer_logic() override {}

    void on_order(const utilities::OrderData& order) override {
        // Contract: ExecutionEngine has already cached the latest order before strategy callback.
        auto* cached = engine_ ? engine_->get_order(order.orderid) : nullptr;
        ASSERT_NE(cached, nullptr);
        EXPECT_EQ(cached->status, order.status);
        saw_order_ = true;
    }

    void on_trade(const utilities::TradeData& trade) override {
        // Contract: PositionEngine has already applied the trade before strategy callback.
        ASSERT_NE(holding_, nullptr);
        if (trade.symbol.size() >= 4 && trade.symbol.substr(trade.symbol.size() - 4) == ".STK") {
            EXPECT_EQ(holding_->underlyingPosition.quantity, 1);
        }
        saw_trade_ = true;
    }

    bool saw_order() const { return saw_order_; }
    bool saw_trade() const { return saw_trade_; }

  private:
    bool saw_order_ = false;
    bool saw_trade_ = false;
};

TEST(BacktestDispatchContract, ExecutionAndPositionAreUpdatedBeforeStrategyCallbacks) {
    // Register test strategy factory at runtime (no production registry edits).
    strategy_cpp::StrategyRegistry::add_factory(
        "DispatchContractTestStrategy",
        [](void* e, const std::string& sn, const std::string& pn,
           const std::unordered_map<std::string, double>& s) -> void* {
            return new DispatchContractTestStrategy(static_cast<core::OptionStrategyEngine*>(e), sn,
                                                    pn, s);
        });

    backtest::MainEngine main;
    ASSERT_NE(main.option_strategy_engine(), nullptr);
    ASSERT_NE(main.execution_engine(), nullptr);
    ASSERT_NE(main.position_engine(), nullptr);

    const std::string portfolio = "P";
    ASSERT_NE(main.portfolio_structure(), nullptr);
    main.portfolio_structure()->ensure_portfolio(portfolio);
    main.option_strategy_engine()->add_strategy("DispatchContractTestStrategy", portfolio, {});
    auto* strat = static_cast<DispatchContractTestStrategy*>(
        main.option_strategy_engine()->get_strategy("DispatchContractTestStrategy_" + portfolio));
    ASSERT_NE(strat, nullptr);

    const std::string oid = "OID-1";
    const std::string strategy_name = "DispatchContractTestStrategy_" + portfolio;
    main.execution_engine()->register_active_order(strategy_name, oid);

    // Order event
    auto* ord = main.acquire_order();
    ASSERT_NE(ord, nullptr);
    ord->orderid = oid;
    ord->symbol = "AAPL.STK";
    ord->status = Status::NOTTRADED;
    main.put_event(Event(EventType::Order, ord));
    EXPECT_TRUE(strat->saw_order());

    // Trade event
    auto* tr = main.acquire_trade();
    ASSERT_NE(tr, nullptr);
    tr->orderid = oid;
    tr->tradeid = "TID-1";
    tr->symbol = "AAPL.STK";
    tr->direction = Direction::LONG;
    tr->price = 100.0;
    tr->volume = 1;
    main.put_event(Event(EventType::Trade, tr));
    EXPECT_TRUE(strat->saw_trade());
}

} // namespace

