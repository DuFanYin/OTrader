/**
 * IB contracts in the platform's terms: the symbols the contract sync saves and fills come back
 * under, and the expiries a sync asks IB for.
 */
#include "adapters/ib/ib_contracts.hpp"
#include "model/utility.hpp"
#include <chrono>
#include <gtest/gtest.h>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace std::chrono;

auto spxw_call(double strike) -> ContractDetails {
    ContractDetails d;
    d.contract.conId = 123;
    d.contract.symbol = "SPX";
    d.contract.secType = "OPT";
    d.contract.lastTradeDateOrContractMonth = "20261016";
    d.contract.strike = strike;
    d.contract.right = "C";
    d.contract.multiplier = "100";
    d.contract.exchange = "SMART";
    d.contract.currency = "USD";
    d.contract.localSymbol = "SPXW  261016C05800000";
    d.contract.tradingClass = "SPXW";
    d.minTick = 0.05;
    return d;
}

TEST(IbContracts, AnOptionIsNamedByItsTradingClass) {
    const model::ContractData c = adapters::option_contract(spxw_call(5800), "SPX");
    EXPECT_EQ(c.symbol, "SPXW-20261016-C-5800");
    EXPECT_EQ(c.option_strike, 5800.0);
    EXPECT_EQ(c.option_type, model::OptionType::CALL);
    EXPECT_EQ(c.option_expiry, model::option_expiry_time(2026y / October / 16));
    EXPECT_EQ(c.option_underlying, "SPX");
    EXPECT_EQ(c.name, "SPXW  261016C05800000");
    EXPECT_EQ(c.product, model::Product::OPTION);
    EXPECT_EQ(c.con_id, 123);
    EXPECT_EQ(c.trading_class, "SPXW");
    EXPECT_DOUBLE_EQ(c.size, 100.0);
    EXPECT_DOUBLE_EQ(c.pricetick, 0.05);

    EXPECT_EQ(adapters::option_contract(spxw_call(582.5), "SPX").symbol, "SPXW-20261016-C-582.5");
}

TEST(IbContracts, Underlyings) {
    ContractDetails spy;
    spy.contract.symbol = "SPY";
    spy.contract.secType = "STK";
    spy.contract.exchange = "SMART";
    spy.contract.currency = "USD";
    const model::ContractData stock = adapters::underlying_contract(spy);
    EXPECT_EQ(stock.symbol, "SPY");
    EXPECT_EQ(stock.product, model::Product::EQUITY);

    ContractDetails spx;
    spx.contract.symbol = "SPX";
    spx.contract.secType = "IND";
    spx.contract.exchange = "CBOE";
    spx.contract.currency = "USD";
    const model::ContractData index = adapters::underlying_contract(spx);
    EXPECT_EQ(index.symbol, "SPX");
    EXPECT_EQ(index.product, model::Product::INDEX);
}

TEST(IbContracts, PlatformSymbolsTranslateToIbAndBack) {
    const Contract option = adapters::ib_contract("SPXW-20261016-P-5800");
    EXPECT_EQ(option.symbol, "SPX"); // the underlying IB lists SPXW under
    EXPECT_EQ(option.secType, "OPT");
    EXPECT_EQ(option.tradingClass, "SPXW");
    EXPECT_EQ(option.lastTradeDateOrContractMonth, "20261016");
    EXPECT_EQ(option.right, "P");
    EXPECT_DOUBLE_EQ(option.strike, 5800.0);
    EXPECT_EQ(adapters::platform_symbol(option), "SPXW-20261016-P-5800");

    const Contract index = adapters::ib_contract("SPX");
    EXPECT_EQ(index.secType, "IND");
    EXPECT_EQ(index.exchange, "CBOE");
    EXPECT_EQ(adapters::platform_symbol(index), "SPX");
    const Contract stock = adapters::ib_contract("QQQ"); // not listed: a stock on SMART
    EXPECT_EQ(stock.secType, "STK");
    EXPECT_EQ(stock.exchange, "SMART");
}

TEST(IbContracts, ExpiriesFromTodayToMaxDteDaysOn) {
    const std::set<std::string> listed{"20261009", "20261010", "20261012", "20261017", "20261019"};
    const year_month_day today = 2026y / October / 10;
    EXPECT_EQ(adapters::expiries_within(listed, today, 7),
              (std::vector<std::string>{"20261010", "20261012", "20261017"}));
    EXPECT_EQ(adapters::expiries_within(listed, today, 0), (std::vector<std::string>{"20261010"}));
}

} // namespace
