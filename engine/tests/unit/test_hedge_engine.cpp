/**
 * The HedgeEngine's plan: hedge a strategy's delta back to its target in whole shares when it
 * leaves the range, and never stack a hedge on one still working. Shares of the underlying, or of
 * what trades in its place (SPY for SPX) at equal value, which the holding then counts in the
 * underlying's units.
 */
#include "core/engines/execution.hpp"
#include "core/engines/hedge.hpp"
#include "core/engines/position.hpp"
#include "model/portfolio_structure.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using model::Direction;

struct Hedge : ::testing::Test {
    Hedge() {
        spy.symbol = "SPY";
        spy.size = 1.0;
        portfolio.underlying = std::make_unique<model::UnderlyingData>(spy);
        structure.ensure_portfolio("contracts"); // where the hedge finds SPY's contract
        structure.process_underlying_for_portfolio("contracts", spy);
    }

    /** The orders a hedge of `delta` sends, within `range` of 0. */
    auto hedge(double delta, int range) -> std::vector<model::OrderRequest> {
        engine.register_strategy("s", "backtest", 1, 0, range);
        holding.summary.delta = delta;
        std::vector<model::OrderRequest> out;
        engine.process_hedging("s", portfolio, holding, &out, &cancels);
        return out;
    }

    /** Sends and cancels nothing: the plan is read from process_hedging's vectors. */
    struct NoRuntime : core::RuntimeAPI {
        std::string send_order(const std::string&, const model::OrderRequest&) override {
            return {};
        }
        void cancel_order(const model::CancelRequest&) override {}
        void subscribe_chains(const std::string&, std::span<const std::string>) override {}
        void unsubscribe_chains(const std::string&) override {}
        bool is_strategy_name_used(const std::string&) override { return false; }
    };

    // The plan (process_hedging) values the holding in `portfolio`; the contracts come from
    // `structure`, the orders working from `execution`.
    model::PortfolioStructure structure;
    core::PositionEngine positions;
    core::ExecutionEngine execution;
    NoRuntime runtime;
    core::HedgeEngine engine{nullptr, structure, positions, execution, runtime};
    model::ContractData spy;
    model::PortfolioData portfolio{"backtest"};
    model::StrategyHolding holding;
    std::vector<model::CancelRequest> cancels;
};

TEST_F(Hedge, NothingWithinTheRange) {
    EXPECT_TRUE(hedge(19.9, 20).empty());
    EXPECT_TRUE(hedge(-20.0, 20).empty());
}

TEST_F(Hedge, BackToZeroInWholeShares) {
    const auto out = hedge(23.51, 20);
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].symbol, "SPY");
    EXPECT_EQ(out[0].direction, Direction::SHORT);
    EXPECT_EQ(out[0].volume, 24.0); // the nearest whole share, not 23.51
    EXPECT_EQ(out[0].type, model::OrderType::MARKET);
    EXPECT_EQ(out[0].reference, "Hedge_s");

    EXPECT_EQ(hedge(-0.6, 0).at(0).volume, 1.0);
    EXPECT_EQ(hedge(-0.6, 0).at(0).direction, Direction::LONG);
    EXPECT_TRUE(hedge(-0.4, 0).empty()); // rounds to no share
}

TEST_F(Hedge, ThroughZeroTheWholeAmount) {
    holding.underlyingPosition.quantity = 10; // long 10 shares, delta +30: sell 30
    double sold = 0.0;
    for (const auto& o : hedge(30.0, 0)) {
        EXPECT_EQ(o.direction, Direction::SHORT);
        sold += o.volume;
    }
    EXPECT_EQ(sold, 30.0);
}

TEST_F(Hedge, AWorkingHedgeIsCancelledNotStacked) {
    model::OrderData working;
    working.orderid = "7";
    working.reference = "Hedge_s";
    working.status = model::Status::NOTTRADED;
    execution.register_active_order("s", "7");
    execution.process_order_event("s", working);

    EXPECT_TRUE(hedge(50.0, 0).empty());
    ASSERT_EQ(cancels.size(), 1U);
    EXPECT_EQ(cancels[0].orderid, "7");
}

TEST_F(Hedge, AnIndexIsHedgedWithWhatTradesAtEqualValue) {
    model::ContractData spx;
    spx.symbol = "SPX";
    portfolio.underlying = std::make_unique<model::UnderlyingData>(spx);
    portfolio.set_hedge_instrument(spy);
    EXPECT_TRUE(hedge(50.0, 0).empty()); // no price for SPY yet: no hedge

    model::PortfolioSnapshot quotes; // SPX at 6000, SPY at 600: a share is a tenth of SPX
    quotes.underlying_mark = 6000.0;
    quotes.hedge_bid = 599.99;
    quotes.hedge_ask = 600.01;
    quotes.hedge_mark = 600.0;
    portfolio.apply_frame(quotes);
    const auto out = hedge(50.0, 0); // long 50 units of SPX
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].symbol, "SPY");
    EXPECT_EQ(out[0].direction, Direction::SHORT);
    EXPECT_EQ(out[0].volume, 500.0);
}

TEST_F(Hedge, NothingWithoutTheInstrumentToHedgeWith) {
    model::ContractData spx;
    spx.symbol = "SPX";
    portfolio.underlying = std::make_unique<model::UnderlyingData>(spx);
    portfolio.hedge_symbol = "SPY"; // what it hedges with, its contract not loaded
    EXPECT_EQ(portfolio.hedge(), nullptr);
    EXPECT_TRUE(hedge(50.0, 0).empty()); // not the underlying in its place
}

TEST(HedgeHolding, SharesOfTheHedgeCountInTheUnderlyingsUnits) {
    model::PortfolioData spxw("SPXW");
    model::ContractData spx;
    spx.symbol = "SPX";
    model::ContractData spy;
    spy.symbol = "SPY";
    spxw.underlying = std::make_unique<model::UnderlyingData>(spx);
    spxw.set_hedge_instrument(spy);
    model::PortfolioSnapshot quotes;
    quotes.underlying_mark = 6000.0;
    quotes.hedge_mark = 600.0;
    spxw.apply_frame(quotes);

    core::PositionEngine positions;
    model::TradeData sold;
    sold.symbol = "SPY";
    sold.orderid = "1";
    sold.tradeid = "1";
    sold.direction = Direction::SHORT;
    sold.price = 600.0;
    sold.volume = 500;
    positions.process_trade_event("s", sold, nullptr);
    positions.update_metrics("s", spxw);

    const model::StrategyHolding& h = positions.get_holding("s");
    EXPECT_EQ(h.underlyingPosition.symbol, "SPY"); // named by its fill
    EXPECT_EQ(h.underlyingPosition.quantity, -500);
    EXPECT_DOUBLE_EQ(h.summary.delta, -50.0);           // in SPX units
    EXPECT_DOUBLE_EQ(h.summary.current_value, -300000); // at SPY's price
    EXPECT_DOUBLE_EQ(h.summary.pnl, 0.0);
}

} // namespace
