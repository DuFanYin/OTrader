/** model::filled_order: a MARKET order of one contract filled whole at once, and its one fill;
 * StrategyHolding::max_loss: what the options held can lose at most. */
#include "model/constant.hpp"
#include "model/object.hpp"
#include <chrono>
#include <cmath>
#include <string>
#include <tuple>
#include <vector>
#include <gtest/gtest.h>

namespace {

using model::Side;

TEST(FilledOrder, IsAMarketOrderTradedWholeWithOneFillNamedByTheOrder) {
    const model::DateTime at = model::DateTime{} + std::chrono::milliseconds(1'760'000'000'123);
    const auto [order, fill] = model::filled_order("OID-1", "SPY-20991218-C-500",
                                                   Side::SELL, 3, 2.45, at, "Settlement");
    EXPECT_EQ(order.order_id, "OID-1");
    EXPECT_EQ(order.symbol, "SPY-20991218-C-500");
    EXPECT_EQ(order.type, model::OrderType::MARKET);
    ASSERT_EQ(order.legs.size(), 1U); // the contract alone
    EXPECT_EQ(order.legs.front().symbol, order.symbol);
    EXPECT_EQ(order.legs.front().side, Side::SELL);
    EXPECT_EQ(order.status, model::Status::FILLED);
    EXPECT_DOUBLE_EQ(order.quantity, 3.0);
    EXPECT_DOUBLE_EQ(order.filled, order.quantity);
    EXPECT_DOUBLE_EQ(order.price, -2.45); // received
    EXPECT_EQ(order.datetime, at);
    EXPECT_EQ(order.reference, "Settlement");
    EXPECT_FALSE(order.is_combo());
    EXPECT_FALSE(order.is_active());

    EXPECT_EQ(fill.order_id, "OID-1");
    EXPECT_EQ(fill.fill_id, "OID-1");
    EXPECT_EQ(fill.symbol, order.symbol);
    EXPECT_EQ(fill.side, Side::SELL);
    EXPECT_DOUBLE_EQ(fill.price, 2.45);
    EXPECT_DOUBLE_EQ(fill.quantity, 3.0);
    EXPECT_EQ(fill.datetime, at);
}

/** symbol, contracts (bought +, sold -), price paid. */
auto holding(std::initializer_list<std::tuple<std::string, int, double>> legs) -> model::StrategyHolding {
    model::StrategyHolding h;
    for (const auto& [symbol, quantity, price] : legs) {
        model::OptionPosition& p = h.options.try_emplace(symbol, symbol, 100.0).first->second;
        p.quantity = quantity;
        p.avg_cost = price;
    }
    return h;
}

TEST(MaxLoss, IsWhatTheOptionsHeldCanLoseAtMost) {
    EXPECT_DOUBLE_EQ(holding({{"SPY-20250303-C-600", 1, 2.5}}).max_loss(), 250.0); // the premium
    // a put spread sold for 1.2: the width less the credit
    EXPECT_NEAR(holding({{"SPY-20250303-P-600", -1, 3.0}, {"SPY-20250303-P-595", 1, 1.8}}).max_loss(),
                380.0, 1e-9);
    // an iron condor sold for 2: the wider wing less the credit
    EXPECT_NEAR(holding({{"SPY-20250303-P-590", 1, 0.5},
                         {"SPY-20250303-P-595", -1, 1.5},
                         {"SPY-20250303-C-605", -1, 1.5},
                         {"SPY-20250303-C-610", 1, 0.5}})
                    .max_loss(),
                300.0, 1e-9);
    EXPECT_TRUE(std::isnan(holding({{"SPY-20250303-C-600", -1, 2.5}}).max_loss())); // no bound
    // legs on two expiries: what it paid
    EXPECT_NEAR(holding({{"SPY-20250303-C-600", -1, 1.0}, {"SPY-20250307-C-600", 1, 2.0}}).max_loss(),
                100.0, 1e-9);
    EXPECT_TRUE(std::isnan(model::StrategyHolding{}.max_loss())); // nothing held
}

} // namespace
