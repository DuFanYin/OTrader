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

TEST(PositionEngineDataflow, SerializeLoadRoundTripPreservesKeyFields) {
    engines::PositionEngine pe;
    const std::string strat = "s1";

    // Underlying trades (open + partial close) => avg_cost + realized_pnl.
    pe.process_trade_event(strat, make_trade("OID-U", "TID-U1", "AAPL.STK", Direction::LONG, 100.0, 10));
    pe.process_trade_event(strat, make_trade("OID-U", "TID-U2", "AAPL.STK", Direction::SHORT, 120.0, 4));

    // Single-leg option (multiplier 100) => avg_cost + realized_pnl.
    pe.process_trade_event(strat, make_trade("OID-O", "TID-O1", "SPXW_20250804C05000000",
                                            Direction::LONG, 1.00, 2));
    pe.process_trade_event(strat, make_trade("OID-O", "TID-O2", "SPXW_20250804C05000000",
                                            Direction::SHORT, 1.50, 1));

    const auto& h1 = pe.get_holding(strat);
    const int u_qty = h1.underlyingPosition.quantity;
    const double u_avg = h1.underlyingPosition.avg_cost;
    const double u_rlz = h1.underlyingPosition.realized_pnl;
    auto it1 = h1.optionPositions.find("SPXW_20250804C05000000");
    ASSERT_NE(it1, h1.optionPositions.end());
    const int o_qty = it1->second.quantity;
    const double o_avg = it1->second.avg_cost;
    const double o_rlz = it1->second.realized_pnl;
    const double o_mult = it1->second.multiplier;

    std::string blob = pe.serialize_holding(strat);
    ASSERT_FALSE(blob.empty());

    engines::PositionEngine pe2;
    pe2.load_serialized_holding(strat, blob);

    const auto& h2 = pe2.get_holding(strat);
    EXPECT_EQ(h2.underlyingPosition.quantity, u_qty);
    EXPECT_DOUBLE_EQ(h2.underlyingPosition.avg_cost, u_avg);
    EXPECT_DOUBLE_EQ(h2.underlyingPosition.realized_pnl, u_rlz);

    auto it2 = h2.optionPositions.find("SPXW_20250804C05000000");
    ASSERT_NE(it2, h2.optionPositions.end());
    EXPECT_EQ(it2->second.quantity, o_qty);
    EXPECT_DOUBLE_EQ(it2->second.avg_cost, o_avg);
    EXPECT_DOUBLE_EQ(it2->second.realized_pnl, o_rlz);
    EXPECT_DOUBLE_EQ(it2->second.multiplier, o_mult);
}

} // namespace

