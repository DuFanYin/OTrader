/**
 * Combo builder helper implementation (migrated from engine_combo_builder).
 */

#include "combo_builder.hpp"
#include <algorithm>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace utilities {
namespace combo {

namespace {

Leg create_leg_impl(const OptionData& option, Direction direction, int volume,
                   const ComboGetContractFn& get_contract,
                   std::optional<double> price) {
    const ContractData* contract = get_contract ? get_contract(option.symbol) : nullptr;
    if (contract == nullptr) {
        throw std::runtime_error("Contract not found for option: " + option.symbol);
    }
    Leg leg;
    leg.con_id = contract->con_id.value_or(0);
    leg.symbol = contract->symbol;
    leg.exchange = contract->exchange;
    leg.direction = direction;
    leg.ratio = volume;
    leg.price = price;
    leg.gateway_name = "IB";
    leg.trading_class = contract->trading_class;
    return leg;
}

std::pair<std::vector<Leg>, std::string>
single_leg(const std::unordered_map<std::string, OptionData*>& option_data,
           Direction direction, int volume, const ComboBuildOptions& opts) {
    if (option_data.size() != 1U) {
        throw std::runtime_error("single_leg requires exactly one option");
    }
    auto it = option_data.begin();
    std::vector<Leg> legs = {create_leg_impl(*it->second, direction, volume, opts.get_contract, std::nullopt)};
    return {legs, generate_combo_signature(legs)};
}

std::pair<std::vector<Leg>, std::string>
straddle(const std::unordered_map<std::string, OptionData*>& option_data,
         Direction direction, int volume, const ComboBuildOptions& opts) {
    auto it_c = option_data.find("call");
    auto it_p = option_data.find("put");
    if (it_c == option_data.end() || it_p == option_data.end()) {
        throw std::runtime_error("straddle requires 'call' and 'put'");
    }
    std::vector<Leg> legs = {
        create_leg_impl(*it_c->second, direction, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*it_p->second, direction, volume, opts.get_contract, std::nullopt)};
    return {legs, generate_combo_signature(legs)};
}

std::pair<std::vector<Leg>, std::string>
strangle(const std::unordered_map<std::string, OptionData*>& option_data,
         Direction direction, int volume, const ComboBuildOptions& opts) {
    auto it_c = option_data.find("call");
    auto it_p = option_data.find("put");
    if (it_c == option_data.end() || it_p == option_data.end()) {
        throw std::runtime_error("strangle requires 'call' and 'put'");
    }
    std::vector<Leg> legs = {
        create_leg_impl(*it_c->second, direction, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*it_p->second, direction, volume, opts.get_contract, std::nullopt)};
    return {legs, generate_combo_signature(legs)};
}

std::pair<std::vector<Leg>, std::string>
iron_condor(const std::unordered_map<std::string, OptionData*>& option_data,
            Direction direction, int volume, const ComboBuildOptions& opts) {
    int sign = (direction == Direction::SHORT) ? 1 : -1;
    auto* pl = option_data.at("put_lower");
    auto* pu = option_data.at("put_upper");
    auto* cl = option_data.at("call_lower");
    auto* cu = option_data.at("call_upper");
    std::vector<Leg> legs = {
        create_leg_impl(*pl, sign > 0 ? Direction::LONG : Direction::SHORT, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*pu, sign > 0 ? Direction::SHORT : Direction::LONG, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*cl, sign > 0 ? Direction::SHORT : Direction::LONG, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*cu, sign > 0 ? Direction::LONG : Direction::SHORT, volume, opts.get_contract, std::nullopt),
    };
    return {legs, generate_combo_signature(legs)};
}

std::pair<std::vector<Leg>, std::string>
risk_reversal(const std::unordered_map<std::string, OptionData*>& option_data,
              Direction direction, int volume, const ComboBuildOptions& opts) {
    int sign = (direction == Direction::SHORT) ? 1 : -1;
    auto* ll = option_data.at("long_leg");
    auto* sl = option_data.at("short_leg");
    std::vector<Leg> legs = {
        create_leg_impl(*ll, sign > 0 ? Direction::LONG : Direction::SHORT, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*sl, sign > 0 ? Direction::SHORT : Direction::LONG, volume, opts.get_contract, std::nullopt),
    };
    return {legs, generate_combo_signature(legs)};
}

std::pair<std::vector<Leg>, std::string>
custom(const std::unordered_map<std::string, OptionData*>& option_data,
       Direction direction, int volume, const ComboBuildOptions& opts) {
    std::vector<Leg> legs;
    legs.reserve(option_data.size());
for (const auto& kv : option_data) {
        legs.push_back(create_leg_impl(*kv.second, direction, volume, opts.get_contract, std::nullopt));
    }
    return {legs, generate_combo_signature(legs)};
}

std::pair<std::vector<Leg>, std::string>
spread(const std::unordered_map<std::string, OptionData*>& option_data,
       Direction direction, int volume, const ComboBuildOptions& opts) {
    int sign = (direction == Direction::LONG) ? 1 : -1;
    auto* ll = option_data.at("long_leg");
    auto* sl = option_data.at("short_leg");
    std::vector<Leg> legs = {
        create_leg_impl(*ll, sign > 0 ? Direction::LONG : Direction::SHORT, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*sl, sign > 0 ? Direction::SHORT : Direction::LONG, volume, opts.get_contract, std::nullopt),
    };
    return {legs, generate_combo_signature(legs)};
}

std::pair<std::vector<Leg>, std::string>
diagonal_spread(const std::unordered_map<std::string, OptionData*>& option_data,
                Direction direction, int volume, const ComboBuildOptions& opts) {
    int sign = (direction == Direction::LONG) ? 1 : -1;
    auto* ll = option_data.at("long_leg");
    auto* sl = option_data.at("short_leg");
    std::vector<Leg> legs = {
        create_leg_impl(*ll, sign > 0 ? Direction::LONG : Direction::SHORT, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*sl, sign > 0 ? Direction::SHORT : Direction::LONG, volume, opts.get_contract, std::nullopt),
    };
    return {legs, generate_combo_signature(legs)};
}

std::pair<std::vector<Leg>, std::string>
ratio_spread(const std::unordered_map<std::string, OptionData*>& option_data,
             Direction direction, int volume, const ComboBuildOptions& opts) {
    int sign = (direction == Direction::LONG) ? 1 : -1;
    int ratio = 2;
    auto* ll = option_data.at("long_leg");
    auto* sl = option_data.at("short_leg");
    std::vector<Leg> legs = {
        create_leg_impl(*ll, sign > 0 ? Direction::LONG : Direction::SHORT, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*sl, sign > 0 ? Direction::SHORT : Direction::LONG, volume * ratio, opts.get_contract, std::nullopt),
    };
    return {legs, generate_combo_signature(legs)};
}

std::pair<std::vector<Leg>, std::string>
butterfly(const std::unordered_map<std::string, OptionData*>& option_data,
          Direction direction, int volume, const ComboBuildOptions& opts) {
    int sign = (direction == Direction::LONG) ? 1 : -1;
    auto* body = option_data.at("body");
    auto* w1 = option_data.at("wing1");
    auto* w2 = option_data.at("wing2");
    std::vector<Leg> legs = {
        create_leg_impl(*body, sign > 0 ? Direction::LONG : Direction::SHORT, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*w1, sign > 0 ? Direction::SHORT : Direction::LONG, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*w2, sign > 0 ? Direction::SHORT : Direction::LONG, volume, opts.get_contract, std::nullopt),
    };
    return {legs, generate_combo_signature(legs)};
}

std::pair<std::vector<Leg>, std::string>
inverse_butterfly(const std::unordered_map<std::string, OptionData*>& option_data,
                  Direction direction, int volume, const ComboBuildOptions& opts) {
    int sign = (direction == Direction::LONG) ? 1 : -1;
    auto* body = option_data.at("body");
    auto* w1 = option_data.at("wing1");
    auto* w2 = option_data.at("wing2");
    std::vector<Leg> legs = {
        create_leg_impl(*body, sign > 0 ? Direction::SHORT : Direction::LONG, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*w1, sign > 0 ? Direction::LONG : Direction::SHORT, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*w2, sign > 0 ? Direction::LONG : Direction::SHORT, volume, opts.get_contract, std::nullopt),
    };
    return {legs, generate_combo_signature(legs)};
}

std::pair<std::vector<Leg>, std::string>
iron_butterfly(const std::unordered_map<std::string, OptionData*>& option_data,
               Direction direction, int volume, const ComboBuildOptions& opts) {
    int sign = (direction == Direction::LONG) ? 1 : -1;
    auto* pw = option_data.at("put_wing");
    auto* body = option_data.at("body");
    auto* cw = option_data.at("call_wing");
    std::vector<Leg> legs = {
        create_leg_impl(*pw, sign > 0 ? Direction::LONG : Direction::SHORT, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*body, sign > 0 ? Direction::SHORT : Direction::LONG, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*cw, sign > 0 ? Direction::LONG : Direction::SHORT, volume, opts.get_contract, std::nullopt),
    };
    return {legs, generate_combo_signature(legs)};
}

std::pair<std::vector<Leg>, std::string>
condor(const std::unordered_map<std::string, OptionData*>& option_data,
       Direction direction, int volume, const ComboBuildOptions& opts) {
    int sign = (direction == Direction::LONG) ? 1 : -1;
    auto* lp = option_data.at("long_put");
    auto* sp = option_data.at("short_put");
    auto* sc = option_data.at("short_call");
    auto* lc = option_data.at("long_call");
    std::vector<Leg> legs = {
        create_leg_impl(*lp, sign > 0 ? Direction::LONG : Direction::SHORT, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*sp, sign > 0 ? Direction::SHORT : Direction::LONG, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*sc, sign > 0 ? Direction::SHORT : Direction::LONG, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*lc, sign > 0 ? Direction::LONG : Direction::SHORT, volume, opts.get_contract, std::nullopt),
    };
    return {legs, generate_combo_signature(legs)};
}

std::pair<std::vector<Leg>, std::string>
box_spread(const std::unordered_map<std::string, OptionData*>& option_data,
           Direction direction, int volume, const ComboBuildOptions& opts) {
    int sign = (direction == Direction::LONG) ? 1 : -1;
    auto* lc = option_data.at("long_call");
    auto* sc = option_data.at("short_call");
    auto* sp = option_data.at("short_put");
    auto* lp = option_data.at("long_put");
    std::vector<Leg> legs = {
        create_leg_impl(*lc, sign > 0 ? Direction::LONG : Direction::SHORT, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*sc, sign > 0 ? Direction::SHORT : Direction::LONG, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*sp, sign > 0 ? Direction::SHORT : Direction::LONG, volume, opts.get_contract, std::nullopt),
        create_leg_impl(*lp, sign > 0 ? Direction::LONG : Direction::SHORT, volume, opts.get_contract, std::nullopt),
    };
    return {legs, generate_combo_signature(legs)};
}

} // namespace

std::pair<std::vector<Leg>, std::string> build_combo(
    const std::unordered_map<std::string, OptionData*>& option_data,
    ComboType combo_type, Direction direction, int volume,
    const ComboBuildOptions& options) {
    switch (combo_type) {
        case ComboType::SINGLE_LEG:
            return single_leg(option_data, direction, volume, options);
        case ComboType::STRADDLE:
            return straddle(option_data, direction, volume, options);
        case ComboType::STRANGLE:
            return strangle(option_data, direction, volume, options);
        case ComboType::IRON_CONDOR:
            return iron_condor(option_data, direction, volume, options);
        case ComboType::RISK_REVERSAL:
            return risk_reversal(option_data, direction, volume, options);
        case ComboType::SPREAD:
            return spread(option_data, direction, volume, options);
        case ComboType::DIAGONAL_SPREAD:
            return diagonal_spread(option_data, direction, volume, options);
        case ComboType::RATIO_SPREAD:
            return ratio_spread(option_data, direction, volume, options);
        case ComboType::BUTTERFLY:
            return butterfly(option_data, direction, volume, options);
        case ComboType::INVERSE_BUTTERFLY:
            return inverse_butterfly(option_data, direction, volume, options);
        case ComboType::IRON_BUTTERFLY:
            return iron_butterfly(option_data, direction, volume, options);
        case ComboType::CONDOR:
            return condor(option_data, direction, volume, options);
        case ComboType::BOX_SPREAD:
            return box_spread(option_data, direction, volume, options);
        case ComboType::CUSTOM:
            return custom(option_data, direction, volume, options);
        default:
            throw std::runtime_error("Unsupported combo type");
    }
}

Leg create_leg(const OptionData& option, Direction direction, int volume,
               const ComboGetContractFn& get_contract,
               std::optional<double> price) {
    return create_leg_impl(option, direction, volume, get_contract, price);
}

std::string generate_combo_signature(std::span<const Leg> legs) {
    std::vector<std::string> parts;
    for (const auto& leg : legs) {
        std::string sym = leg.symbol.value_or("");
        if (sym.empty()) {
            continue;
        }
        std::vector<std::string> tokens;
        for (auto part : sym | std::views::split('-')) {
            tokens.emplace_back(part.begin(), part.end());
        }
        if (tokens.size() >= 4U) {
            parts.push_back(tokens[1] + tokens[2] + tokens[3]);
        }
    }
    std::ranges::sort(parts);
    std::string result;
    bool first = true;
    for (const auto& p : parts) {
        if (!std::exchange(first, false)) {
            result += "-";
        }
        result += p;
    }
    return result;
}

} // namespace combo
} // namespace utilities
