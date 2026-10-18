#include "model/order_builder.hpp"
#include "model/symbol.hpp"
#include "model/utility.hpp"
#include <algorithm>
#include <cstdlib>
#include <format>
#include <numeric>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace model {

namespace {

Side opposite(Side side) {
    return side == Side::BUY ? Side::SELL : Side::BUY;
}

} // namespace

std::pair<std::vector<Leg>, std::string>
build_combo(const std::unordered_map<std::string, std::string>& roles, ComboType combo_type,
            Side side, const ComboGetContractFn& find_contract) {
    std::vector<Leg> legs;
    // buy: the leg takes the combo's side, else the opposite. Legs are sized per combo unit;
    // the order's quantity multiplies Leg.ratio in the gateways (backtest leg fill = quantity *
    // ratio; IB ComboLeg.ratio with totalQuantity = quantity).
    const auto leg = [&](const std::string& role, bool buy, int ratio = 1) {
        legs.push_back(create_leg(roles.at(role), buy ? side : opposite(side), ratio,
                                  find_contract));
    };
    for (const auto& [role, symbol] : roles) {
        if (symbol.empty()) {
            throw std::runtime_error("No option for " + role);
        }
    }
    switch (combo_type) {
    case ComboType::SINGLE_LEG:
        if (roles.size() != 1U) {
            throw std::runtime_error("single_leg requires exactly one option");
        }
        legs.push_back(create_leg(roles.begin()->second, side, 1, find_contract));
        break;
    case ComboType::STRADDLE:
        if (!roles.contains("call") || !roles.contains("put")) {
            throw std::runtime_error("straddle requires 'call' and 'put'");
        }
        leg("call", true);
        leg("put", true);
        break;
    case ComboType::STRANGLE:
        if (!roles.contains("call") || !roles.contains("put")) {
            throw std::runtime_error("strangle requires 'call' and 'put'");
        }
        leg("call", true);
        leg("put", true);
        break;
    case ComboType::IRON_CONDOR:
        leg("put_lower", false);
        leg("put_upper", true);
        leg("call_lower", true);
        leg("call_upper", false);
        break;
    case ComboType::RISK_REVERSAL:
    case ComboType::SPREAD:
    case ComboType::DIAGONAL_SPREAD:
        leg("long_leg", true);
        leg("short_leg", false);
        break;
    case ComboType::RATIO_SPREAD:
        leg("long_leg", true);
        leg("short_leg", false, 2);
        break;
    case ComboType::BUTTERFLY:
        leg("body", true);
        leg("wing1", false);
        leg("wing2", false);
        break;
    case ComboType::INVERSE_BUTTERFLY:
        leg("body", false);
        leg("wing1", true);
        leg("wing2", true);
        break;
    case ComboType::IRON_BUTTERFLY:
        leg("put_wing", true);
        leg("body", false);
        leg("call_wing", true);
        break;
    case ComboType::CONDOR:
        leg("long_put", true);
        leg("short_put", false);
        leg("short_call", false);
        leg("long_call", true);
        break;
    case ComboType::BOX_SPREAD:
        leg("long_call", true);
        leg("short_call", false);
        leg("short_put", false);
        leg("long_put", true);
        break;
    case ComboType::CUSTOM:
        legs.reserve(roles.size());
        for (const auto& [_, symbol] : roles) {
            legs.push_back(create_leg(symbol, side, 1, find_contract));
        }
        break;
    default:
        throw std::runtime_error("Unsupported combo type");
    }
    std::string signature = generate_combo_signature(legs);
    return {std::move(legs), std::move(signature)};
}

Leg create_leg(const std::string& symbol, Side side, int ratio,
               const ComboGetContractFn& find_contract, std::optional<double> price) {
    const Contract* contract = find_contract ? find_contract(symbol) : nullptr;
    if (contract == nullptr) {
        throw std::runtime_error("Contract not found for option: " + symbol);
    }
    Leg leg;
    leg.con_id = contract->con_id.value_or(0);
    leg.symbol = contract->symbol;
    leg.side = side;
    leg.ratio = ratio;
    leg.price = price;
    leg.trading_class = contract->trading_class;
    return leg;
}

std::string generate_combo_signature(std::span<const Leg> legs) {
    std::vector<std::string> options;
    for (const auto& leg : legs) {
        if (parse_option_symbol(leg.symbol)) {
            options.push_back(leg.symbol);
        }
    }
    std::ranges::sort(options);
    std::string signature;
    for (const std::string& symbol : options) {
        signature += (signature.empty() ? "" : "|") + symbol;
    }
    return signature;
}


Leg contract_leg(const Contract& contract, Side side) {
    return {.con_id = contract.con_id.value_or(0),
            .ratio = 1,
            .side = side,
            .symbol = contract.symbol,
            .trading_class = contract.trading_class};
}

OrderRequest order_request(std::span<const Leg> legs, double price, double quantity,
                           OrderType order_type, ComboType combo_type) {
    OrderRequest req{.type = order_type,
                     .quantity = round_to(quantity, 1.0), // whole units
                     .price = order_type == OrderType::MARKET ? 0.0 : round_to(price, 0.01),
                     .trading_class = legs.empty() ? std::string{} : legs.front().trading_class,
                     .legs = {legs.begin(), legs.end()}};
    req.combo_type = req.is_combo() ? combo_type : ComboType::SINGLE_LEG;
    req.symbol = req.is_combo() ? to_string(combo_type) + "_" + generate_combo_signature(legs)
                                : legs.front().symbol;
    return req;
}

auto closing_order(std::span<const OptionPosition> options, const ComboGetContractFn& find_contract)
    -> std::expected<OrderRequest, std::string> {
    int units = 0;
    for (const OptionPosition& p : options) {
        units = std::gcd(units, p.quantity);
    }
    std::vector<Leg> legs;
    for (const OptionPosition& p : options) {
        if (p.quantity == 0) {
            continue;
        }
        const Contract* contract = find_contract(p.symbol);
        if (contract == nullptr) {
            return std::unexpected(std::format("No contract {}: not closed", p.symbol));
        }
        Leg& leg =
            legs.emplace_back(contract_leg(*contract, p.quantity > 0 ? Side::SELL : Side::BUY));
        leg.ratio = std::abs(p.quantity) / units;
    }
    if (legs.empty()) {
        return std::unexpected(std::string("Nothing open to close"));
    }
    return order_request(legs, 0.0, units, OrderType::MARKET, ComboType::CUSTOM);
}

} // namespace model
