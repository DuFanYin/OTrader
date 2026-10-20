/**
 * A strategy's delta hedge: back to its target in whole shares when it leaves the range, never
 * stacked on a hedge still working. Shares of the underlying, or of what trades in its place (SPY
 * for SPX) at equal value. Once a bar, every timer_trigger bars, whatever the strategy's state.
 */
#include "core/bookkeeping/bookkeeping_engine.hpp"
#include "core/strategy/strategy_engine.hpp"
#include "model/portfolios.hpp"
#include "strategy/strategy_registry.hpp"
#include "strategy/strategy.hpp"
#include "utilities/logger.hpp"
#include <chrono>
#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono;
using model::Side;

class HedgeProbe final : public strategy::Strategy {
  public:
    using Strategy::Strategy;

    void on_timer() override {}
};

REGISTER_STRATEGY(HedgeProbe);

struct Recorder final : utilities::Logger {
    void log(const utilities::LogRecord& record) override { records.push_back(record); }
    std::vector<utilities::LogRecord> records;
};

/** An index whose hedge instrument is not loaded. */
constexpr model::PortfolioSpec kUnhedged{.name = "UNHEDGED",
                                         .underlying = "SPX",
                                         .underlying_product = model::Product::INDEX,
                                         .hedge = "DIA"};

/** One strategy, "s", on a portfolio of no spec, "S", whose underlying is SPY (what hedges trade
 * until it is given a hedge instrument); what its hedging sends. */
struct Hedge : ::testing::Test {
    Hedge() {
        spy.symbol = "SPY";
        spy.multiplier = 1.0;
        structure.ensure_portfolio("S");
        structure.ensure_portfolio("bare"); // no contract loaded
        portfolio().set_underlying(spy);
        engine.add_strategy({.name = "s", .class_name = "HedgeProbe", .portfolio = "S"});
    }

    model::Portfolio& portfolio(const std::string& name = "S") {
        return *structure.find_portfolio(name);
    }
    strategy::Strategy& strategy(const std::string& name = "s") {
        return *engine.find_strategy(name);
    }
    void delta(double d, const std::string& name = "s") {
        book.ensure_holding(name).summary.delta = d;
    }
    /** The next bar: the portfolio's quotes as they are, a minute on. */
    auto bar(const std::string& name = "S") -> std::vector<model::OrderRequest> {
        model::PortfolioSnapshot moved = portfolio(name).snapshot();
        moved.assign(portfolio(name).frame());
        moved.datetime = open + minutes(bars++);
        portfolio(name).apply(moved);
        engine.run_hedging();
        return std::exchange(sent, {});
    }
    /** What hedging `delta`, within `range` of 0, sends on the next bar. */
    auto hedge(double d, int range) -> std::vector<model::OrderRequest> {
        strategy().register_hedging(1, 0, range);
        delta(d);
        return bar();
    }
    void working(const std::string& order_id, const std::string& reference) {
        model::Order order;
        order.order_id = order_id;
        order.reference = reference;
        order.status = model::Status::WORKING;
        book.add_order("s", order);
    }

    const model::DateTime open = sys_days{2025y / March / 3} + 14h + 30min; // 09:30 New York
    int bars = 0;
    Recorder logger;
    model::Contract spy;
    model::Portfolios structure;
    core::BookkeepingEngine book{nullptr, structure};
    std::vector<model::OrderRequest> sent;
    std::vector<model::CancelRequest> cancels;
    core::StrategyEngine engine{&logger, structure, book,
                                {.send_order =
                                     [this](const model::StrategyRecord&, const model::OrderRequest& r) {
                                         sent.push_back(r);
                                         return std::to_string(sent.size());
                                     },
                                 .cancel_order =
                                     [this](const model::CancelRequest& c) {
                                         cancels.push_back(c);
                                     },
                                 .state_changed = {}}};
};

TEST_F(Hedge, NothingWithinTheRange) {
    EXPECT_TRUE(hedge(19.9, 20).empty());
    EXPECT_TRUE(hedge(-20.0, 20).empty());
}

TEST_F(Hedge, BackToZeroInWholeShares) {
    const auto out = hedge(23.51, 20);
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].symbol, "SPY");
    EXPECT_EQ(out[0].legs.front().side, Side::SELL);
    EXPECT_EQ(out[0].quantity, 24.0); // the nearest whole share, not 23.51
    EXPECT_EQ(out[0].type, model::OrderType::MARKET);
    EXPECT_EQ(out[0].reference, "Hedge_s");

    EXPECT_EQ(hedge(-0.6, 0).at(0).quantity, 1.0);
    EXPECT_EQ(hedge(-0.6, 0).at(0).legs.front().side, Side::BUY);
    EXPECT_TRUE(hedge(-0.4, 0).empty()); // rounds to no share
}

TEST_F(Hedge, ThroughZeroTheWholeAmount) {
    book.ensure_holding("s").shares.quantity = 10; // delta +30: sell 30
    const auto out = hedge(30.0, 0);
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].legs.front().side, Side::SELL);
    EXPECT_EQ(out[0].quantity, 30.0);
}

TEST_F(Hedge, AWorkingHedgeIsCancelledNotStacked) {
    working("7", "Hedge_s");
    EXPECT_TRUE(hedge(50.0, 0).empty());
    ASSERT_EQ(cancels.size(), 1U);
    EXPECT_EQ(cancels[0].order_id, "7");
}

TEST_F(Hedge, TheStrategysOwnOrderIsNoHedge) {
    working("8", "Strategy_HedgedStraddle_SPY"); // a strategy whose name has "Hedge" in it
    EXPECT_EQ(hedge(50.0, 0).size(), 1U);         // hedged, its own order left working
    EXPECT_TRUE(cancels.empty());
}

TEST_F(Hedge, AnotherClientsOrderIsNoHedgeToCancel) {
    book.update_order(
        {.order_id = "X1", .status = model::Status::WORKING, .reference = "Hedge_s"}, {});
    EXPECT_EQ(hedge(50.0, 0).size(), 1U);
    EXPECT_TRUE(cancels.empty());
}

TEST_F(Hedge, AnIndexIsHedgedWithWhatTradesAtEqualValue) {
    model::Contract spx;
    spx.symbol = "SPX";
    portfolio().set_underlying(spx);
    portfolio().set_hedge_instrument(spy);
    EXPECT_TRUE(hedge(50.0, 0).empty()); // no price for SPY yet: no hedge

    // SPX at 6000, SPY at 600: a share is a tenth of SPX.
    model::PortfolioSnapshot quotes = portfolio().snapshot();
    quotes.underlying_mark = 6000.0;
    quotes.hedge_bid = 599.99;
    quotes.hedge_ask = 600.01;
    quotes.hedge_mark = 600.0;
    quotes.priced = true;
    portfolio().apply(quotes);
    const auto out = hedge(50.0, 0); // long 50 units of SPX
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].symbol, "SPY");
    EXPECT_EQ(out[0].legs.front().side, Side::SELL);
    EXPECT_EQ(out[0].quantity, 500.0);
}

TEST_F(Hedge, NothingWithoutTheInstrumentToHedgeWith) {
    model::Contract spx;
    spx.symbol = "SPX";
    structure.ensure_portfolio(kUnhedged);
    structure.load_contracts(std::vector{spx}); // DIA's not loaded
    EXPECT_FALSE(portfolio("UNHEDGED").hedge());
    engine.add_strategy({.name = "u", .class_name = "HedgeProbe", .portfolio = "UNHEDGED"});
    strategy("u").register_hedging(1, 0, 0);
    delta(50.0, "u");
    EXPECT_TRUE(bar("UNHEDGED").empty()); // not the underlying in its place
}

TEST_F(Hedge, EveryTriggerBarsOnceABar) {
    strategy().register_hedging(2, 0, 10);
    delta(30.0);
    EXPECT_TRUE(bar().empty()); // the first bar of two
    --bars;
    EXPECT_TRUE(bar().empty()); // the same bar again
    const auto out = bar();
    ASSERT_EQ(out.size(), 1U);
    EXPECT_EQ(out[0].legs.front().side, Side::SELL);
    EXPECT_EQ(out[0].quantity, 30.0);
    EXPECT_TRUE(bar().empty()); // counting again from the hedge
}

TEST_F(Hedge, OnlyAStrategyThatHedgesWhileItDoes) {
    engine.add_strategy(
        {.name = "other", .class_name = "HedgeProbe", .portfolio = "S"}); // never hedges
    delta(50.0, "other");
    EXPECT_TRUE(bar().empty());
    EXPECT_EQ(engine.hedging("other"), nullptr);

    strategy().register_hedging(1, 0, 10);
    ASSERT_NE(engine.hedging("s"), nullptr);
    EXPECT_EQ(engine.hedging("s")->delta_range, 10);
    delta(-11.0);
    EXPECT_EQ(bar().size(), 1U); // added, not even inited: what it holds is hedged
    strategy().unregister_hedging();
    EXPECT_EQ(engine.hedging("s"), nullptr);
    EXPECT_TRUE(bar().empty());
    EXPECT_TRUE(cancels.empty());
}

TEST_F(Hedge, ItsHedgingGoesWithTheStrategy) {
    strategy().register_hedging(1, 0, 0);
    ASSERT_TRUE(engine.remove_strategy("s"));
    engine.restore_strategy({.name = "s", .class_name = "HedgeProbe", .portfolio = "S"});
    EXPECT_EQ(engine.hedging("s"), nullptr);
}

TEST_F(Hedge, APortfolioWithNothingToHedgeWithIsWarnedOfAsTheStrategy) {
    engine.add_strategy({.name = "t", .class_name = "HedgeProbe", .portfolio = "bare"});
    logger.records.clear();
    strategy().register_hedging();
    EXPECT_TRUE(logger.records.empty());
    strategy("t").register_hedging();
    ASSERT_EQ(logger.records.size(), 1U);
    EXPECT_EQ(logger.records[0].level, utilities::WARNING);
    EXPECT_EQ(logger.records[0].source, "t");
    EXPECT_NE(logger.records[0].msg.find("no underlying contract"), std::string::npos)
        << logger.records[0].msg;
    EXPECT_NE(engine.hedging("t"), nullptr); // registered all the same
}

} // namespace
