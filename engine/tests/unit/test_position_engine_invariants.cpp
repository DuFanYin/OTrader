#include "core/engines/position.hpp"
#include "model/constant.hpp"
#include "model/object.hpp"
#include "model/portfolio.hpp"
#include "model/symbol.hpp"
#include <chrono>
#include <memory>
#include <gtest/gtest.h>
#include <tuple>
#include <vector>
 
namespace {
 
using model::Direction;
using model::TradeData;
 
static TradeData make_trade(std::string orderid, std::string tradeid, std::string symbol,
                            Direction dir, double price, double volume) {
    TradeData t;
    t.orderid = std::move(orderid);
    t.tradeid = std::move(tradeid);
    t.symbol = std::move(symbol);
    t.direction = dir;
    t.price = price;
    t.volume = volume;
    return t;
}
 
/** A combo order over these legs (one of each per combo). */
static model::OrderData combo_order(std::string id, Direction d,
                                    std::vector<std::pair<std::string, Direction>> legs) {
    model::OrderData o;
    o.orderid = std::move(id);
    o.direction = d;
    o.is_combo = true;
    for (auto& [symbol, side] : legs) {
        model::Leg l;
        l.symbol = symbol;
        l.direction = side;
        l.ratio = 1;
        o.symbol += (o.symbol.empty() ? "combo_" : "|") + symbol;
        o.legs.push_back(l);
    }
    return o;
}

TEST(PositionEngineInvariants, UnderlyingAvgCostAndRealizedPnlOnCloseAndReverse) {
    core::PositionEngine pe;
    const std::string strat = "s1";
    auto buy10_100 = make_trade("OID", "TID-1", "AAPL", Direction::LONG, 100.0, 10);
    auto buy10_110 = make_trade("OID", "TID-2", "AAPL", Direction::LONG, 110.0, 10);
    pe.process_trade_event(strat, buy10_100, nullptr);
    pe.process_trade_event(strat, buy10_110, nullptr);
 
    // Avg cost = (10*100 + 10*110)/20 = 105.00
    auto& h1 = pe.get_holding(strat);
    EXPECT_EQ(h1.underlyingPosition.quantity, 20);
    EXPECT_DOUBLE_EQ(h1.underlyingPosition.avg_cost, 105.0);
 
    // Sell 5 @120 => realized = (120-105)*5 = 75
    auto sell5_120 = make_trade("OID", "TID-3", "AAPL", Direction::SHORT, 120.0, 5);
    pe.process_trade_event(strat, sell5_120, nullptr);
    auto& h2 = pe.get_holding(strat);
    EXPECT_EQ(h2.underlyingPosition.quantity, 15);
    EXPECT_DOUBLE_EQ(h2.underlyingPosition.realized_pnl, 75.0);
    EXPECT_DOUBLE_EQ(h2.underlyingPosition.avg_cost, 105.0);
 
    // Sell 25 @90 closes remaining 15 (loss) then opens short 10 @90
    // Close PnL = (90-105)*15 = -225
    auto sell25_90 = make_trade("OID", "TID-4", "AAPL", Direction::SHORT, 90.0, 25);
    pe.process_trade_event(strat, sell25_90, nullptr);
    auto& h3 = pe.get_holding(strat);
    EXPECT_EQ(h3.underlyingPosition.quantity, -10);
    EXPECT_DOUBLE_EQ(h3.underlyingPosition.avg_cost, 90.0);
    EXPECT_DOUBLE_EQ(h3.underlyingPosition.realized_pnl, 75.0 - 225.0);
}
 
TEST(PositionEngineInvariants, AFillIsAnOptionWhenItsSymbolNamesOne) {
    core::PositionEngine pe;
    const std::string strat = "s1";

    pe.process_trade_event(strat, make_trade("OID", "TID-1", "SPY", Direction::LONG, 580.0, 3), nullptr);
    pe.process_trade_event(
        strat, make_trade("OID", "TID-2", "SPY-20991218-P-600", Direction::LONG, 2.0, 1), nullptr);

    const auto& h = pe.get_holding(strat);
    EXPECT_EQ(h.underlyingPosition.quantity, 3);
    EXPECT_FALSE(h.optionPositions.contains("SPY"));
    EXPECT_TRUE(h.optionPositions.contains("SPY-20991218-P-600"));
}

TEST(PositionEngineInvariants, SingleLegOptionUsesMultiplierAndRealizedPnl) {
    core::PositionEngine pe;
    const std::string strat = "s1";
 
    // Buy 2 contracts @1.00, then sell 1 @1.50 => realized = (1.5-1.0)*1*100 = 50
    auto b2 = make_trade("OID", "TID-1", "SPXW-20250804-C-5000", Direction::LONG, 1.00, 2);
    pe.process_trade_event(strat, b2, nullptr);
    auto s1 = make_trade("OID", "TID-2", "SPXW-20250804-C-5000", Direction::SHORT, 1.50, 1);
    pe.process_trade_event(strat, s1, nullptr);
 
    const auto& h = pe.get_holding(strat);
    auto it = h.optionPositions.find("SPXW-20250804-C-5000");
    ASSERT_NE(it, h.optionPositions.end());
    EXPECT_EQ(it->second.quantity, 1);
    EXPECT_DOUBLE_EQ(it->second.avg_cost, 1.00);
    EXPECT_DOUBLE_EQ(it->second.multiplier, 100.0);
    EXPECT_DOUBLE_EQ(it->second.realized_pnl, 50.0);
}
 
static model::OrderData make_combo_order(std::string orderid, std::string symbol,
                                             model::ComboType type,
                                             std::vector<std::string> const& leg_symbols) {
    model::OrderData o;
    o.orderid = std::move(orderid);
    o.symbol = std::move(symbol);
    o.is_combo = true;
    o.combo_type = type;
    std::vector<model::Leg> legs;
    int con_id = 1;
    for (auto const& sym : leg_symbols) {
        model::Leg l;
        l.con_id = con_id++;
        l.ratio = 1;
        l.direction = Direction::LONG;
        l.symbol = sym;
        legs.push_back(l);
    }
    o.legs = legs;
    return o;
}

// A straddle opened by one order and closed by another of another type over the same legs: the
// same position, its type the opening order's.
TEST(PositionEngineInvariants, ComboClosedUnderAnotherPrefixClosesSamePosition) {
    core::PositionEngine pe;
    const std::string strat = "s1";
    const std::string call = "SPXW-20250804-CALL-6325-100";
    const std::string put = "SPXW-20250804-PUT-6325-100";

    auto open = make_combo_order("OID-1", "straddle_SIG", model::ComboType::STRADDLE,
                                 {call, put});
    pe.process_trade_event(strat, make_trade("OID-1", "T1-0", call, Direction::LONG, 3.50, 1),
                           &open);
    pe.process_trade_event(strat, make_trade("OID-1", "T1-1", put, Direction::LONG, 27.70, 1),
                           &open);

    auto close =
        make_combo_order("OID-2", "custom_SIG", model::ComboType::CUSTOM, {put, call});
    pe.process_trade_event(strat, make_trade("OID-2", "T2-0", put, Direction::SHORT, 24.60, 1),
                           &close);
    pe.process_trade_event(strat, make_trade("OID-2", "T2-1", call, Direction::SHORT, 3.90, 1),
                           &close);

    const auto& h = pe.get_holding(strat);
    ASSERT_EQ(h.optionPositions.size(), 1U);
    const auto& pos = h.optionPositions.begin()->second;
    ASSERT_TRUE(pos.combo_type.has_value());
    EXPECT_EQ(*pos.combo_type, model::ComboType::STRADDLE);
    EXPECT_EQ(pos.quantity, 0);
    double realized = 0.0;
    for (const auto& leg : pos.legs) {
        EXPECT_EQ(leg.quantity, 0) << leg.symbol;
        realized += leg.realized_pnl;
    }
    // (24.60 - 27.70 + 3.90 - 3.50) * 100
    EXPECT_NEAR(realized, -270.0, 1e-9);
}

// cost_value is signed like current_value(): a short's premium is a credit, so
// unrealized = current_value() - cost_value is right for both directions.
TEST(PositionEngineInvariants, ShortCostValueIsSignedSoUnrealizedPnlIsCorrect) {
    core::PositionEngine pe;
    const std::string strat = "s1";
    const std::string sym = "SPXW-20250804-C-5000";
    pe.process_trade_event(strat, make_trade("OID", "T1", sym, Direction::SHORT, 2.00, 1), nullptr);

    auto& pos = pe.get_holding(strat).optionPositions.at(sym);
    EXPECT_EQ(pos.quantity, -1);
    EXPECT_DOUBLE_EQ(pos.cost_value, -200.0);
    pos.mid_price = 1.50; // option fell: the short made 50
    EXPECT_DOUBLE_EQ(pos.current_value() - pos.cost_value, 50.0);
}

/** A combo order fills as its legs; the combo is its legs (keyed by their symbols, sorted), its
 * quantity theirs over each leg's ratio, whatever order fills first. */
TEST(PositionEngineInvariants, ComboQuantityIsComputedFromLegs) {
    core::PositionEngine pe;
    const std::string strat = "s";
    const std::string call = "SPY-20991218-C-600";
    const std::string put = "SPY-20991218-P-600";
    const std::string low = "SPY-20991218-C-601";
    const std::string high = "SPY-20991218-C-603";
    const auto held = [&](const std::string& key) {
        return pe.get_holding(strat).optionPositions.at(key);
    };
    const std::string straddle_key = call + "|" + put;
    const std::string spread_key = low + "|" + high;

    // Two straddles bought: two of each leg.
    const auto straddle =
        combo_order("1", Direction::LONG, {{call, Direction::LONG}, {put, Direction::LONG}});
    pe.process_trade_event(strat, make_trade("1", "1a", call, Direction::LONG, 5.0, 2), &straddle);
    EXPECT_EQ(held(straddle_key).quantity, 2) << "computed from the first booked leg";
    pe.process_trade_event(strat, make_trade("1", "1b", put, Direction::LONG, 4.0, 2), &straddle);
    EXPECT_EQ(held(straddle_key).quantity, 2);
    EXPECT_EQ(held(straddle_key).legs.size(), 2U);

    // A call spread sold: the low call sold, the high one bought.
    const auto spread =
        combo_order("2", Direction::SHORT, {{low, Direction::SHORT}, {high, Direction::LONG}});
    pe.process_trade_event(strat, make_trade("2", "2a", low, Direction::SHORT, 1.0, 1), &spread);
    pe.process_trade_event(strat, make_trade("2", "2b", high, Direction::LONG, 0.4, 1), &spread);
    EXPECT_EQ(held(spread_key).quantity, -1) << "one sold";

    // The straddle closed by another combo order over the same legs (as a strategy closes it).
    const auto closing =
        combo_order("3", Direction::SHORT, {{call, Direction::SHORT}, {put, Direction::SHORT}});
    pe.process_trade_event(strat, make_trade("3", "3a", call, Direction::SHORT, 6.0, 2), &closing);
    pe.process_trade_event(strat, make_trade("3", "3b", put, Direction::SHORT, 3.0, 2), &closing);
    EXPECT_EQ(held(straddle_key).quantity, 0);
    EXPECT_EQ(pe.get_holding(strat).optionPositions.size(), 2U)
        << "an order over the same legs books to the same position";
}

/** The same fills in another order (the closing order's first, legs interleaved) leave the same
 * holding. */
TEST(PositionEngineInvariants, HoldingIsIndependentOfFillOrder) {
    const std::string call = "SPY-20991218-C-600";
    const std::string put = "SPY-20991218-P-600";
    const auto open =
        combo_order("1", Direction::LONG, {{call, Direction::LONG}, {put, Direction::LONG}});
    const auto close =
        combo_order("2", Direction::SHORT, {{call, Direction::SHORT}, {put, Direction::SHORT}});
    const std::vector<std::pair<TradeData, const model::OrderData*>> fills{
        {make_trade("1", "1a", call, Direction::LONG, 5.0, 3), &open},
        {make_trade("1", "1b", put, Direction::LONG, 4.0, 3), &open},
        {make_trade("2", "2a", call, Direction::SHORT, 6.0, 1), &close},
        {make_trade("2", "2b", put, Direction::SHORT, 3.0, 1), &close},
    };
    const auto book = [&](std::vector<int> sequence) {
        core::PositionEngine pe;
        for (int i : sequence) {
            pe.process_trade_event("s", fills[i].first, fills[i].second);
        }
        const auto& p = pe.get_holding("s").optionPositions.at(call + "|" + put);
        return std::tuple{p.quantity, p.legs[0].quantity, p.legs[1].quantity, p.legs[0].cost_value,
                          pe.get_holding("s").optionPositions.size()};
    };
    const auto expected = book({0, 1, 2, 3});
    EXPECT_EQ(std::get<0>(expected), 2);
    EXPECT_EQ(book({2, 3, 0, 1}), expected) << "the closing order's fills first";
    EXPECT_EQ(book({3, 0, 2, 1}), expected) << "legs interleaved";
}

// A call bought above its mid, then the market moves a day on: the fill's cost against the mid,
// then the move by the Greeks the position had, with what they do not explain as other.
TEST(PositionEngineInvariants, PnlIsAttributedToItsSourcesAndAddsUp) {
    using namespace std::chrono;
    core::PositionEngine pe;
    const std::string strat = "s1";
    const std::string call = model::option_symbol({.root = "SPY",
                                                   .expiry = 2099y / December / 18,
                                                   .type = model::OptionType::CALL,
                                                   .strike = 600});
    model::PortfolioData portfolio("SPY");
    model::ContractData spy;
    spy.symbol = "SPY";
    spy.size = 1.0;
    portfolio.underlying = std::make_unique<model::UnderlyingData>(spy);
    model::OptionData& option = portfolio.options[call];
    option.symbol = call;
    const auto mark = [&](double underlying, double mid, double iv, sys_days day) {
        portfolio.underlying->mid_price = underlying;
        option.mid_price = mid;
        option.mid_iv = iv;
        option.delta = 50.0; // per contract, as priced: 0.5 x 100
        option.gamma = 2.0;
        option.theta = -5.0; // USD a day
        option.vega = 20.0;  // USD a vol point
        portfolio.datetime_ = day;
        pe.update_metrics(strat, portfolio);
    };

    pe.get_create_strategy_holding(strat);
    mark(600.0, 10.0, 0.20, sys_days{2099y / January / 5}); // the first update: nothing held
    pe.process_trade_event(strat, make_trade("OID", "T1", call, Direction::LONG, 10.5, 2), nullptr);
    mark(600.0, 10.0, 0.20, sys_days{2099y / January / 5});
    const model::PortfolioSummary& first = pe.get_holding(strat).summary;
    EXPECT_DOUBLE_EQ(first.pnl, -100.0);
    EXPECT_DOUBLE_EQ(first.pnl_fills, -100.0); // 2 x (10.0 - 10.5) x 100

    mark(602.0, 11.2, 0.21, sys_days{2099y / January / 6});
    const model::PortfolioSummary& s = pe.get_holding(strat).summary;
    EXPECT_NEAR(s.pnl_delta, 200.0, 1e-9); // 2 x 50 x 2
    EXPECT_NEAR(s.pnl_gamma, 8.0, 1e-9);   // 0.5 x 2 x 2 x 2^2
    EXPECT_NEAR(s.pnl_theta, -10.0, 1e-9); // 2 x -5 x 1 day
    EXPECT_NEAR(s.pnl_vega, 40.0, 1e-9);   // 2 x 20 x 1 point
    EXPECT_NEAR(s.pnl_other, 2.0, 1e-9);   // 2 x 1.2 x 100 = 240, 238 explained
    EXPECT_NEAR(s.pnl_fills, -100.0, 1e-9);
    EXPECT_NEAR(s.pnl_delta + s.pnl_gamma + s.pnl_theta + s.pnl_vega + s.pnl_other + s.pnl_fills,
                s.pnl, 1e-9);
    EXPECT_NEAR(s.pnl, 140.0, 1e-9);
}

} // namespace
