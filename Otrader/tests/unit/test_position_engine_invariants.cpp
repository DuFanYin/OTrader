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
 
} // namespace

