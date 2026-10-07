#include "core/engine_position.hpp"
#include "utilities/constant.hpp"
#include "utilities/object.hpp"
#include <gtest/gtest.h>
 
namespace {
 
using utilities::Direction;
using utilities::TradeData;
 
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
 
TEST(PositionEngineInvariants, DuplicateTradeIdIsIgnored) {
    engines::PositionEngine pe;
    const std::string strat = "s1";
 
    TradeData t1 = make_trade("OID", "TID-1", "AAPL.STK", Direction::LONG, 100.0, 10);
    pe.process_trade_event(strat, t1);
    pe.process_trade_event(strat, t1); // duplicate
 
    const auto& h = pe.get_holding(strat);
    EXPECT_EQ(h.underlyingPosition.quantity, 10);
    EXPECT_DOUBLE_EQ(h.underlyingPosition.avg_cost, 100.0);
}
 
TEST(PositionEngineInvariants, UnderlyingAvgCostAndRealizedPnlOnCloseAndReverse) {
    engines::PositionEngine pe;
    const std::string strat = "s1";
    auto buy10_100 = make_trade("OID", "TID-1", "AAPL.STK", Direction::LONG, 100.0, 10);
    auto buy10_110 = make_trade("OID", "TID-2", "AAPL.STK", Direction::LONG, 110.0, 10);
    pe.process_trade_event(strat, buy10_100);
    pe.process_trade_event(strat, buy10_110);
 
    // Avg cost = (10*100 + 10*110)/20 = 105.00
    auto& h1 = pe.get_holding(strat);
    EXPECT_EQ(h1.underlyingPosition.quantity, 20);
    EXPECT_DOUBLE_EQ(h1.underlyingPosition.avg_cost, 105.0);
 
    // Sell 5 @120 => realized = (120-105)*5 = 75
    auto sell5_120 = make_trade("OID", "TID-3", "AAPL.STK", Direction::SHORT, 120.0, 5);
    pe.process_trade_event(strat, sell5_120);
    auto& h2 = pe.get_holding(strat);
    EXPECT_EQ(h2.underlyingPosition.quantity, 15);
    EXPECT_DOUBLE_EQ(h2.underlyingPosition.realized_pnl, 75.0);
    EXPECT_DOUBLE_EQ(h2.underlyingPosition.avg_cost, 105.0);
 
    // Sell 25 @90 closes remaining 15 (loss) then opens short 10 @90
    // Close PnL = (90-105)*15 = -225
    auto sell25_90 = make_trade("OID", "TID-4", "AAPL.STK", Direction::SHORT, 90.0, 25);
    pe.process_trade_event(strat, sell25_90);
    auto& h3 = pe.get_holding(strat);
    EXPECT_EQ(h3.underlyingPosition.quantity, -10);
    EXPECT_DOUBLE_EQ(h3.underlyingPosition.avg_cost, 90.0);
    EXPECT_DOUBLE_EQ(h3.underlyingPosition.realized_pnl, 75.0 - 225.0);
}
 
TEST(PositionEngineInvariants, SingleLegOptionUsesMultiplierAndRealizedPnl) {
    engines::PositionEngine pe;
    const std::string strat = "s1";
 
    // Buy 2 contracts @1.00, then sell 1 @1.50 => realized = (1.5-1.0)*1*100 = 50
    auto b2 = make_trade("OID", "TID-1", "SPXW_20250804C05000000", Direction::LONG, 1.00, 2);
    pe.process_trade_event(strat, b2);
    auto s1 = make_trade("OID", "TID-2", "SPXW_20250804C05000000", Direction::SHORT, 1.50, 1);
    pe.process_trade_event(strat, s1);
 
    const auto& h = pe.get_holding(strat);
    auto it = h.optionPositions.find("SPXW_20250804C05000000");
    ASSERT_NE(it, h.optionPositions.end());
    EXPECT_EQ(it->second.quantity, 1);
    EXPECT_DOUBLE_EQ(it->second.avg_cost, 1.00);
    EXPECT_DOUBLE_EQ(it->second.multiplier, 100.0);
    EXPECT_DOUBLE_EQ(it->second.realized_pnl, 50.0);
}
 
TEST(PositionEngineInvariants, ComboOrderRoutesHeadAndLegTrades) {
    engines::PositionEngine pe;
    const std::string strat = "s1";
 
    utilities::OrderData o;
    o.orderid = "OID-C";
    o.symbol = "COMBO-SPXW";
    o.is_combo = true;
    o.combo_type = utilities::ComboType::STRADDLE;
    utilities::Leg l1;
    l1.con_id = 1;
    l1.exchange = utilities::Exchange::LOCAL;
    l1.ratio = 1;
    l1.direction = Direction::LONG;
    l1.symbol = "LEG-CALL";
    utilities::Leg l2;
    l2.con_id = 2;
    l2.exchange = utilities::Exchange::LOCAL;
    l2.ratio = 1;
    l2.direction = Direction::LONG;
    l2.symbol = "LEG-PUT";
    o.legs = std::vector<utilities::Leg>{l1, l2};
    pe.process_order_event(strat, o);
 
    // Head fill
    TradeData head = make_trade(o.orderid, "TID-1", o.symbol, Direction::LONG, 2.00, 1);
    pe.process_trade_event("" /*empty strategy triggers meta lookup*/, head);
 
    // Leg fill
    TradeData leg = make_trade(o.orderid, "TID-2", "LEG-CALL", Direction::LONG, 1.10, 1);
    pe.process_trade_event("" /*empty strategy triggers meta lookup*/, leg);
 
    const auto& h = pe.get_holding(strat);
    auto it = h.optionPositions.find(o.symbol);
    ASSERT_NE(it, h.optionPositions.end());
    EXPECT_TRUE(it->second.combo_type.has_value());
    EXPECT_EQ(it->second.quantity, 1); // head qty updated via option-position overload (no realized pnl)
    ASSERT_FALSE(it->second.legs.empty());
 
    bool found_leg = false;
    for (const auto& lg : it->second.legs) {
        if (lg.symbol == "LEG-CALL") {
            found_leg = true;
            EXPECT_EQ(lg.quantity, 1);
        }
    }
    EXPECT_TRUE(found_leg);
}
 

static utilities::OrderData make_combo_order(std::string orderid, std::string symbol,
                                             utilities::ComboType type,
                                             std::vector<std::string> const& leg_symbols) {
    utilities::OrderData o;
    o.orderid = std::move(orderid);
    o.symbol = std::move(symbol);
    o.is_combo = true;
    o.combo_type = type;
    std::vector<utilities::Leg> legs;
    int con_id = 1;
    for (auto const& sym : leg_symbols) {
        utilities::Leg l;
        l.con_id = con_id++;
        l.exchange = utilities::Exchange::LOCAL;
        l.ratio = 1;
        l.direction = Direction::LONG;
        l.symbol = sym;
        legs.push_back(l);
    }
    o.legs = legs;
    return o;
}

// A straddle opened as "straddle_<sig>" and closed by close_all_strategy_positions as
// "custom_<sig>" (legs listed in another order) must close the same position. Previously the
// type round-trip parsed as CUSTOM and the close opened a second, short combo: quantity never
// reached 0 and the short's cost counted the premium received as paid.
TEST(PositionEngineInvariants, ComboClosedUnderAnotherPrefixClosesSamePosition) {
    engines::PositionEngine pe;
    const std::string strat = "s1";
    const std::string call = "SPXW-20250804-CALL-6325-100";
    const std::string put = "SPXW-20250804-PUT-6325-100";

    auto open = make_combo_order("OID-1", "straddle_SIG", utilities::ComboType::STRADDLE,
                                 {call, put});
    pe.process_order_event(strat, open);
    pe.process_trade_event("",
                           make_trade("OID-1", "T1", "straddle_SIG", Direction::LONG, 31.20, 1));
    pe.process_trade_event("", make_trade("OID-1", "T1-0", call, Direction::LONG, 3.50, 1));
    pe.process_trade_event("", make_trade("OID-1", "T1-1", put, Direction::LONG, 27.70, 1));

    auto close =
        make_combo_order("OID-2", "custom_SIG", utilities::ComboType::CUSTOM, {put, call});
    pe.process_order_event(strat, close);
    pe.process_trade_event("",
                           make_trade("OID-2", "T2", "custom_SIG", Direction::SHORT, 28.50, 1));
    pe.process_trade_event("", make_trade("OID-2", "T2-0", put, Direction::SHORT, 24.60, 1));
    pe.process_trade_event("", make_trade("OID-2", "T2-1", call, Direction::SHORT, 3.90, 1));

    const auto& h = pe.get_holding(strat);
    ASSERT_EQ(h.optionPositions.size(), 1U);
    const auto& pos = h.optionPositions.begin()->second;
    ASSERT_TRUE(pos.combo_type.has_value());
    EXPECT_EQ(*pos.combo_type, utilities::ComboType::STRADDLE);
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
    engines::PositionEngine pe;
    const std::string strat = "s1";
    const std::string sym = "SPXW_20250804C05000000";
    pe.process_trade_event(strat, make_trade("OID", "T1", sym, Direction::SHORT, 2.00, 1));

    auto& pos = pe.get_holding(strat).optionPositions.at(sym);
    EXPECT_EQ(pos.quantity, -1);
    EXPECT_DOUBLE_EQ(pos.cost_value, -200.0);
    pos.mid_price = 1.50; // option fell: the short made 50
    EXPECT_DOUBLE_EQ(pos.current_value() - pos.cost_value, 50.0);
}

} // namespace

