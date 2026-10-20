/**
 * The simulated market's venue (entry_simulate): an order fills on the quotes as the backtest's
 * would, every fill goes into the account on its way to the engine, the account is kept in its file
 * and read back by the next run, an option past its close is settled at its intrinsic value (SPY's
 * in shares), and orders from before a restart are cancelled.
 */
#include "adapters/gateway/simulated_gateway.hpp"
#include "model/session_calendar.hpp"
#include "model/portfolios.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using adapters::SimulatedGateway;

constexpr const char* kCall = "SPY-20991218-C-601";

/** An account value, as the venue writes it: to the cent. */
auto value(const core::BrokerAccount& account, const std::string& key) -> double {
    for (const core::AccountValue& v : account.values) {
        if (v.key == key) {
            return std::stod(v.value);
        }
    }
    return 0.0;
}

auto position(const core::BrokerAccount& account, const std::string& symbol) -> double {
    for (const core::AccountPosition& p : account.positions) {
        if (p.symbol == symbol) {
            return p.quantity;
        }
    }
    return 0.0;
}

auto order(const char* symbol, model::Side side, double quantity) -> model::OrderRequest {
    model::OrderRequest req;
    req.symbol = symbol;
    req.type = model::OrderType::MARKET;
    req.quantity = quantity;
    req.legs = {{.ratio = 1, .side = side, .symbol = symbol}};
    return req;
}

class Simulated : public ::testing::Test {
  protected:
    void SetUp() override {
        std::filesystem::remove(file);
        portfolios.ensure_portfolios_created();
        model::Contract spy;
        spy.symbol = "SPY";
        spy.product = model::Product::EQUITY;
        model::Contract call;
        call.symbol = kCall;
        call.multiplier = 100;
        portfolios.load_contracts(std::vector{spy, call});
        quote(605.0, 1.00, 1.10);
    }
    void TearDown() override { std::filesystem::remove(file); }

    /** SPY at `spot` (a cent wide), the call at bid / ask. */
    void quote(double spot, double bid, double ask) {
        model::Portfolio& p = *portfolios.find_portfolio("SPY");
        model::PortfolioSnapshot s = p.snapshot();
        s.assign(p.frame());
        s.underlying_bid = spot - 0.005;
        s.underlying_ask = spot + 0.005;
        s.underlying_mark = spot;
        const std::uint32_t column = *p.listing().column(kCall);
        s.values(&model::Frame::bid)[column] = bid;
        s.values(&model::Frame::ask)[column] = ask;
        p.apply(s);
    }
    auto venue() -> std::unique_ptr<SimulatedGateway> {
        auto v = std::make_unique<SimulatedGateway>(portfolios, std::vector<std::string>{}, file,
                                                    [this] { return now; });
        v->set_callbacks([this](const model::Order& o) { orders.push_back(o); },
                         [this](const model::Fill& f) { fills.push_back(f); });
        return v;
    }

    const std::string file = ::testing::TempDir() + "optra_simulated_account.json";
    model::Portfolios portfolios;
    model::DateTime now = model::DateTime{} + std::chrono::years(100); // before the call's expiry
    std::vector<model::Order> orders;
    std::vector<model::Fill> fills;
};

TEST_F(Simulated, AFillGoesIntoTheAccountAndTheNextRunReadsItBack) {
    auto v = venue();
    EXPECT_TRUE(v->send_order(order(kCall, model::Side::BUY, 2)).empty()); // not connected
    v->connect();
    ASSERT_FALSE(v->send_order(order(kCall, model::Side::BUY, 2)).empty());
    ASSERT_EQ(fills.size(), 1U); // at once: the quotes allow it
    EXPECT_EQ(fills.front().price, 1.10);
    const core::BrokerAccount bought = v->query_account().get();
    EXPECT_EQ(position(bought, kCall), 2.0);
    // IB's fees: 2 x 0.65 a contract (at least 1 an order).
    EXPECT_DOUBLE_EQ(value(bought, "TotalCashValue"), SimulatedGateway::kStartingCash - 220.0 - 1.3);

    quote(605.0, 1.20, 1.30);
    v->on_minute(); // marked at the mid
    const core::BrokerAccount marked = v->query_account().get();
    EXPECT_DOUBLE_EQ(value(marked, "UnrealizedPnL"), 30.0); // 2 x 100 x (1.25 - 1.10)

    v->send_order(order(kCall, model::Side::SELL, 1)); // at the bid
    const core::BrokerAccount sold = venue()->query_account().get(); // the next run's, from the file
    EXPECT_EQ(position(sold, kCall), 1.0);
    EXPECT_DOUBLE_EQ(value(sold, "RealizedPnL"), 10.0); // 100 x (1.20 - 1.10)
    EXPECT_DOUBLE_EQ(value(sold, "TotalCashValue"),
                     SimulatedGateway::kStartingCash - 220.0 - 1.3 + 120.0 - 1.0); // the least
}

TEST_F(Simulated, AnOptionPastItsCloseIsSettledInShares) {
    auto v = venue();
    v->connect();
    v->send_order(order(kCall, model::Side::BUY, 1));
    now = model::kNyse.expiry_time(model::parse_option_symbol(kCall)->expiry) +
          std::chrono::minutes(1);
    quote(605.0, 0.0, 0.0); // no quote for it after the close
    v->on_minute();
    const core::BrokerAccount settled = v->query_account().get();
    EXPECT_EQ(position(settled, kCall), 0.0);
    EXPECT_EQ(position(settled, "SPY"), 100.0); // exercised: 100 shares at the strike
    // 110 and a fee of 1 paid for it; 4 of value a share in the money, 605 a share paid for the
    // shares.
    EXPECT_DOUBLE_EQ(value(settled, "TotalCashValue"),
                     SimulatedGateway::kStartingCash - 110.0 - 1.0 + 400.0 - 60'500.0);
}

TEST_F(Simulated, OrdersFromBeforeARestartAreCancelled) {
    auto v = venue();
    model::Order before;
    before.order_id = "sim1.7";
    before.status = model::Status::WORKING;
    v->restore_orders(std::vector{before});
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders.front().order_id, "sim1.7");
    EXPECT_EQ(orders.front().status, model::Status::CANCELLED);
}

} // namespace
