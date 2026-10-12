#include "ib_contracts.hpp"
#include "../../model/portfolios.hpp"
#include "../../model/constant.hpp"
#include "../../model/symbol.hpp"
#include "../../model/utility.hpp"
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>

namespace adapters {

namespace {

auto product_of(const std::string& sec_type) -> model::Product {
    if (sec_type == "OPT") {
        return model::Product::OPTION;
    }
    if (sec_type == "IND") {
        return model::Product::INDEX;
    }
    return sec_type == "STK" ? model::Product::EQUITY : model::Product::UNKNOWN;
}

/** IB's YYYYMMDD (an option's may go on with a time) as a date. */
auto date_of(const std::string& yyyymmdd) -> std::optional<std::chrono::year_month_day> {
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    if (std::sscanf(yyyymmdd.c_str(), "%4d%2u%2u", &y, &m, &d) != 3) {
        return std::nullopt;
    }
    const std::chrono::year_month_day date{std::chrono::year{y}, std::chrono::month{m},
                                           std::chrono::day{d}};
    return date.ok() ? std::optional(date) : std::nullopt;
}

auto option_type_of(const std::string& right) -> model::OptionType {
    return right.starts_with('P') ? model::OptionType::PUT : model::OptionType::CALL;
}

/** The ticker IB lists an option root's underlying under: SPXW -> SPX. */
auto underlying_of(const std::string& root) -> std::string {
    const model::PortfolioSpec* portfolio = model::find_portfolio(root);
    return portfolio != nullptr ? std::string(portfolio->underlying) : root;
}

/** What every contract IB describes has. */
auto contract_of(const ContractDetails& details) -> model::ContractData {
    const Contract& c = details.contract;
    model::ContractData out;
    out.symbol = platform_symbol(c);
    out.name = details.longName;
    out.product = product_of(c.secType);
    out.pricetick = details.minTick > 0.0 ? details.minTick : 0.01;
    out.con_id = c.conId;
    if (!c.tradingClass.empty()) {
        out.trading_class = c.tradingClass;
    }
    return out;
}

} // namespace

auto platform_symbol(const Contract& c) -> std::string {
    if (c.secType == "OPT" || c.secType == "FOP") {
        if (const auto expiry = date_of(c.lastTradeDateOrContractMonth)) {
            return model::option_symbol(
                {.root = c.tradingClass.empty() ? c.symbol : c.tradingClass,
                 .expiry = *expiry,
                 .type = option_type_of(c.right),
                 .strike = c.strike});
        }
    }
    if (!c.symbol.empty()) {
        return c.symbol; // an underlying: its ticker
    }
    return c.conId != 0 ? std::to_string(c.conId) : "UNKNOWN";
}

auto ib_contract(const std::string& symbol) -> Contract {
    Contract out;
    out.currency = "USD";
    out.exchange = "SMART";
    if (const auto id = model::parse_option_symbol(symbol)) {
        out.symbol = underlying_of(id->root);
        out.secType = "OPT";
        out.tradingClass = id->root;
        out.lastTradeDateOrContractMonth = std::format("{:%Y%m%d}", id->expiry);
        out.right = id->type == model::OptionType::CALL ? "C" : "P";
        out.strike = id->strike;
        return out;
    }
    out.symbol = symbol;
    const IbListing* listing = find_listing(symbol);
    out.secType = listing != nullptr ? listing->sec_type : "STK";
    if (listing != nullptr) {
        out.exchange = listing->exchange;
    }
    return out;
}

auto ib_combo_contract(std::span<const model::Leg> legs, const std::string& trading_class)
    -> Contract {
    Contract out;
    out.secType = "BAG";
    out.currency = "USD";
    out.exchange = "SMART";
    out.tradingClass = trading_class;
    out.comboLegs = std::make_shared<Contract::ComboLegList>();
    for (const model::Leg& leg : legs) {
        auto cl = std::make_shared<ComboLeg>();
        cl->conId = leg.con_id;
        cl->ratio = std::abs(leg.ratio);
        cl->action = (leg.direction == model::Direction::LONG) ? "BUY" : "SELL";
        cl->exchange = "SMART";
        out.comboLegs->push_back(cl);
        if (out.symbol.empty() && !leg.symbol.empty()) {
            const auto id = model::parse_option_symbol(leg.symbol);
            out.symbol = id ? underlying_of(id->root) : leg.symbol;
        }
    }
    return out;
}

auto underlying_contract(const ContractDetails& details) -> model::ContractData {
    return contract_of(details);
}

auto option_contract(const ContractDetails& details, const std::string& underlying_symbol)
    -> model::ContractData {
    const Contract& c = details.contract;
    model::ContractData out = contract_of(details);
    out.name = c.localSymbol;
    out.size = 100.0;
    std::from_chars(c.multiplier.data(), c.multiplier.data() + c.multiplier.size(), out.size);
    out.option_strike = c.strike;
    out.option_type = option_type_of(c.right);
    if (const auto date = date_of(c.lastTradeDateOrContractMonth)) {
        out.option_expiry = model::option_expiry_time(*date);
    }
    out.option_underlying = underlying_symbol;
    return out;
}

auto expiries_within(const std::set<std::string>& expirations, std::chrono::year_month_day from,
                     int max_dte) -> std::vector<std::string> {
    const std::chrono::year_month_day to{std::chrono::sys_days{from} + std::chrono::days{max_dte}};
    const std::string first = std::format("{:%Y%m%d}", from);
    const std::string last = std::format("{:%Y%m%d}", to);
    std::vector<std::string> out;
    for (const std::string& e : expirations) { // a set: in order
        if (first <= e && e <= last) {
            out.push_back(e);
        }
    }
    return out;
}

} // namespace adapters
