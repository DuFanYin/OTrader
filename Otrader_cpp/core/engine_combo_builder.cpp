/**
 * Shared ComboBuilder implementation; logs output as LogIntent (out_logs).
 */

#include "engine_combo_builder.hpp"
#include <algorithm>
#include <sstream>

namespace engines {

auto ComboBuilderEngine::create_leg(const utilities::OptionData& option,
    utilities::Direction direction, int volume, std::optional<double> price,
    const ComboGetContractFn& get_contract_fn) -> utilities::Leg {
    const utilities::ContractData* contract = nullptr;
    if (get_contract_fn) {
        contract = get_contract_fn(option.symbol);
    } else if (current_get_contract_) {
        contract = current_get_contract_(option.symbol);
}
    if (contract == nullptr) {
        throw std::runtime_error("Contract not found for option: " + option.symbol);
    }
    utilities::Leg leg;
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

auto ComboBuilderEngine::combo_builder(
    const std::unordered_map<std::string, utilities::OptionData*>& option_data,
    utilities::ComboType combo_type, utilities::Direction direction, int volume,
    ComboGetContractFn get_contract_fn, std::vector<utilities::LogData>* out_logs) -> std::pair<std::vector<utilities::Leg>, std::string> {
    current_get_contract_ = std::move(get_contract_fn);
    current_out_logs_ = out_logs;
    auto result = combo_builder_impl(option_data, combo_type, direction, volume);
    current_get_contract_ = nullptr;
    current_out_logs_ = nullptr;
    return result;
}

auto ComboBuilderEngine::combo_builder_impl(
    const std::unordered_map<std::string, utilities::OptionData*>& option_data,
    utilities::ComboType combo_type, utilities::Direction direction, int volume) -> std::pair<std::vector<utilities::Leg>, std::string> {
    switch (combo_type) {
        case utilities::ComboType::STRADDLE: return straddle(option_data, direction, volume);
        case utilities::ComboType::STRANGLE: return strangle(option_data, direction, volume);
        case utilities::ComboType::IRON_CONDOR: return iron_condor(option_data, direction, volume);
        case utilities::ComboType::RISK_REVERSAL: return risk_reversal(option_data, direction, volume);
        case utilities::ComboType::SPREAD: return spread(option_data, direction, volume);
        case utilities::ComboType::DIAGONAL_SPREAD: return diagonal_spread(option_data, direction, volume);
        case utilities::ComboType::RATIO_SPREAD: return ratio_spread(option_data, direction, volume);
        case utilities::ComboType::BUTTERFLY: return butterfly(option_data, direction, volume);
        case utilities::ComboType::INVERSE_BUTTERFLY: return inverse_butterfly(option_data, direction, volume);
        case utilities::ComboType::IRON_BUTTERFLY: return iron_butterfly(option_data, direction, volume);
        case utilities::ComboType::CONDOR: return condor(option_data, direction, volume);
        case utilities::ComboType::BOX_SPREAD: return box_spread(option_data, direction, volume);
        case utilities::ComboType::CUSTOM: return custom(option_data, direction, volume);
        default: throw std::runtime_error("Unsupported combo type");
    }
}

auto ComboBuilderEngine::straddle(
    const std::unordered_map<std::string, utilities::OptionData*>& option_data,
    utilities::Direction direction, int volume) -> std::pair<std::vector<utilities::Leg>, std::string> {
    auto it_c = option_data.find("call");
    auto it_p = option_data.find("put");
    if (it_c == option_data.end() || it_p == option_data.end()) {
        throw std::runtime_error("straddle requires 'call' and 'put'");
}
    std::vector<utilities::Leg> legs = {
        create_leg(*it_c->second, direction, volume),
        create_leg(*it_p->second, direction, volume)
    };
    return {legs, generate_combo_signature(legs)};
}

auto ComboBuilderEngine::strangle(
    const std::unordered_map<std::string, utilities::OptionData*>& option_data,
    utilities::Direction direction, int volume) -> std::pair<std::vector<utilities::Leg>, std::string> {
    auto it_c = option_data.find("call");
    auto it_p = option_data.find("put");
    if (it_c == option_data.end() || it_p == option_data.end()) {
        throw std::runtime_error("strangle requires 'call' and 'put'");
}
    std::vector<utilities::Leg> legs = {
        create_leg(*it_c->second, direction, volume),
        create_leg(*it_p->second, direction, volume)
    };
    return {legs, generate_combo_signature(legs)};
}

auto ComboBuilderEngine::iron_condor(
    const std::unordered_map<std::string, utilities::OptionData*>& option_data,
    utilities::Direction direction, int volume) -> std::pair<std::vector<utilities::Leg>, std::string> {
    int sign = (direction == utilities::Direction::SHORT) ? 1 : -1;
    auto *pl = option_data.at("put_lower");
    auto *pu = option_data.at("put_upper");
    auto *cl = option_data.at("call_lower");
    auto *cu = option_data.at("call_upper");
    std::vector<utilities::Leg> legs = {
        create_leg(*pl, sign > 0 ? utilities::Direction::LONG : utilities::Direction::SHORT, volume),
        create_leg(*pu, sign > 0 ? utilities::Direction::SHORT : utilities::Direction::LONG, volume),
        create_leg(*cl, sign > 0 ? utilities::Direction::SHORT : utilities::Direction::LONG, volume),
        create_leg(*cu, sign > 0 ? utilities::Direction::LONG : utilities::Direction::SHORT, volume),
    };
    return {legs, generate_combo_signature(legs)};
}

auto ComboBuilderEngine::risk_reversal(
    const std::unordered_map<std::string, utilities::OptionData*>& option_data,
    utilities::Direction direction, int volume) -> std::pair<std::vector<utilities::Leg>, std::string> {
    int sign = (direction == utilities::Direction::SHORT) ? 1 : -1;
    auto *ll = option_data.at("long_leg");
    auto *sl = option_data.at("short_leg");
    std::vector<utilities::Leg> legs = {
        create_leg(*ll, sign > 0 ? utilities::Direction::LONG : utilities::Direction::SHORT, volume),
        create_leg(*sl, sign > 0 ? utilities::Direction::SHORT : utilities::Direction::LONG, volume),
    };
    return {legs, generate_combo_signature(legs)};
}

auto ComboBuilderEngine::custom(
    const std::unordered_map<std::string, utilities::OptionData*>& option_data,
    utilities::Direction direction, int volume) -> std::pair<std::vector<utilities::Leg>, std::string> {
    std::vector<utilities::Leg> legs;
    for (const auto& kv : option_data) {
        legs.push_back(create_leg(*kv.second, direction, volume));
        if (current_out_logs_ != nullptr) {
            utilities::LogData log;
            log.msg = "Custom Combo Leg: " + legs.back().symbol.value_or("") +
                " | Direction: " + std::to_string(static_cast<int>(direction)) + " | Volume: " + std::to_string(legs.back().ratio);
            log.level = 10;
            log.gateway_name = "Combo";
            current_out_logs_->push_back(log);
        }
    }
    return {legs, generate_combo_signature(legs)};
}

auto ComboBuilderEngine::spread(
    const std::unordered_map<std::string, utilities::OptionData*>& option_data,
    utilities::Direction direction, int volume) -> std::pair<std::vector<utilities::Leg>, std::string> {
    int sign = (direction == utilities::Direction::LONG) ? 1 : -1;
    auto *ll = option_data.at("long_leg");
    auto *sl = option_data.at("short_leg");
    std::vector<utilities::Leg> legs = {
        create_leg(*ll, sign > 0 ? utilities::Direction::LONG : utilities::Direction::SHORT, volume),
        create_leg(*sl, sign > 0 ? utilities::Direction::SHORT : utilities::Direction::LONG, volume),
    };
    return {legs, generate_combo_signature(legs)};
}

auto ComboBuilderEngine::diagonal_spread(
    const std::unordered_map<std::string, utilities::OptionData*>& option_data,
    utilities::Direction direction, int volume) -> std::pair<std::vector<utilities::Leg>, std::string> {
    int sign = (direction == utilities::Direction::LONG) ? 1 : -1;
    auto *ll = option_data.at("long_leg");
    auto *sl = option_data.at("short_leg");
    std::vector<utilities::Leg> legs = {
        create_leg(*ll, sign > 0 ? utilities::Direction::LONG : utilities::Direction::SHORT, volume),
        create_leg(*sl, sign > 0 ? utilities::Direction::SHORT : utilities::Direction::LONG, volume),
    };
    return {legs, generate_combo_signature(legs)};
}

auto ComboBuilderEngine::ratio_spread(
    const std::unordered_map<std::string, utilities::OptionData*>& option_data,
    utilities::Direction direction, int volume) -> std::pair<std::vector<utilities::Leg>, std::string> {
    int sign = (direction == utilities::Direction::LONG) ? 1 : -1;
    int ratio = 2;  // default 1:2 ratio, same as Python
    auto *ll = option_data.at("long_leg");
    auto *sl = option_data.at("short_leg");
    std::vector<utilities::Leg> legs = {
        create_leg(*ll, sign > 0 ? utilities::Direction::LONG : utilities::Direction::SHORT, volume),
        create_leg(*sl, sign > 0 ? utilities::Direction::SHORT : utilities::Direction::LONG, volume * ratio),
    };
    return {legs, generate_combo_signature(legs)};
}

auto ComboBuilderEngine::butterfly(
    const std::unordered_map<std::string, utilities::OptionData*>& option_data,
    utilities::Direction direction, int volume) -> std::pair<std::vector<utilities::Leg>, std::string> {
    int sign = (direction == utilities::Direction::LONG) ? 1 : -1;
    auto *body = option_data.at("body");
    auto *w1 = option_data.at("wing1");
    auto *w2 = option_data.at("wing2");
    std::vector<utilities::Leg> legs = {
        create_leg(*body, sign > 0 ? utilities::Direction::LONG : utilities::Direction::SHORT, volume),
        create_leg(*w1, sign > 0 ? utilities::Direction::SHORT : utilities::Direction::LONG, volume),
        create_leg(*w2, sign > 0 ? utilities::Direction::SHORT : utilities::Direction::LONG, volume),
    };
    return {legs, generate_combo_signature(legs)};
}

auto ComboBuilderEngine::inverse_butterfly(
    const std::unordered_map<std::string, utilities::OptionData*>& option_data,
    utilities::Direction direction, int volume) -> std::pair<std::vector<utilities::Leg>, std::string> {
    int sign = (direction == utilities::Direction::LONG) ? 1 : -1;
    auto *body = option_data.at("body");
    auto *w1 = option_data.at("wing1");
    auto *w2 = option_data.at("wing2");
    std::vector<utilities::Leg> legs = {
        create_leg(*body, sign > 0 ? utilities::Direction::SHORT : utilities::Direction::LONG, volume),
        create_leg(*w1, sign > 0 ? utilities::Direction::LONG : utilities::Direction::SHORT, volume),
        create_leg(*w2, sign > 0 ? utilities::Direction::LONG : utilities::Direction::SHORT, volume),
    };
    return {legs, generate_combo_signature(legs)};
}

auto ComboBuilderEngine::iron_butterfly(
    const std::unordered_map<std::string, utilities::OptionData*>& option_data,
    utilities::Direction direction, int volume) -> std::pair<std::vector<utilities::Leg>, std::string> {
    int sign = (direction == utilities::Direction::LONG) ? 1 : -1;
    auto *pw = option_data.at("put_wing");
    auto *body = option_data.at("body");
    auto *cw = option_data.at("call_wing");
    std::vector<utilities::Leg> legs = {
        create_leg(*pw, sign > 0 ? utilities::Direction::LONG : utilities::Direction::SHORT, volume),
        create_leg(*body, sign > 0 ? utilities::Direction::SHORT : utilities::Direction::LONG, volume),
        create_leg(*cw, sign > 0 ? utilities::Direction::LONG : utilities::Direction::SHORT, volume),
    };
    return {legs, generate_combo_signature(legs)};
}

auto ComboBuilderEngine::condor(
    const std::unordered_map<std::string, utilities::OptionData*>& option_data,
    utilities::Direction direction, int volume) -> std::pair<std::vector<utilities::Leg>, std::string> {
    int sign = (direction == utilities::Direction::LONG) ? 1 : -1;
    auto *lp = option_data.at("long_put");
    auto *sp = option_data.at("short_put");
    auto *sc = option_data.at("short_call");
    auto *lc = option_data.at("long_call");
    std::vector<utilities::Leg> legs = {
        create_leg(*lp, sign > 0 ? utilities::Direction::LONG : utilities::Direction::SHORT, volume),
        create_leg(*sp, sign > 0 ? utilities::Direction::SHORT : utilities::Direction::LONG, volume),
        create_leg(*sc, sign > 0 ? utilities::Direction::SHORT : utilities::Direction::LONG, volume),
        create_leg(*lc, sign > 0 ? utilities::Direction::LONG : utilities::Direction::SHORT, volume),
    };
    return {legs, generate_combo_signature(legs)};
}

auto ComboBuilderEngine::box_spread(
    const std::unordered_map<std::string, utilities::OptionData*>& option_data,
    utilities::Direction direction, int volume) -> std::pair<std::vector<utilities::Leg>, std::string> {
    int sign = (direction == utilities::Direction::LONG) ? 1 : -1;
    auto *lc = option_data.at("long_call");
    auto *sc = option_data.at("short_call");
    auto *sp = option_data.at("short_put");
    auto *lp = option_data.at("long_put");
    std::vector<utilities::Leg> legs = {
        create_leg(*lc, sign > 0 ? utilities::Direction::LONG : utilities::Direction::SHORT, volume),
        create_leg(*sc, sign > 0 ? utilities::Direction::SHORT : utilities::Direction::LONG, volume),
        create_leg(*sp, sign > 0 ? utilities::Direction::SHORT : utilities::Direction::LONG, volume),
        create_leg(*lp, sign > 0 ? utilities::Direction::LONG : utilities::Direction::SHORT, volume),
    };
    return {legs, generate_combo_signature(legs)};
}

auto ComboBuilderEngine::generate_combo_signature(const std::vector<utilities::Leg>& legs) -> std::string {
    std::vector<std::string> parts;
    for (const auto& leg : legs) {
        std::string sym = leg.symbol.value_or("");
        if (sym.empty()) { continue;
}
        size_t pos = 0;
        std::vector<std::string> tokens;
        for (size_t i = 0; i <= sym.size(); ++i) {
            if (i == sym.size() || sym[i] == '-') {
                if (i > pos) { tokens.push_back(sym.substr(pos, i - pos));
}
                pos = i + 1;
            }
        }
        if (tokens.size() >= 4) {
            parts.push_back(tokens[1] + tokens[2] + tokens[3]);
}
    }
    std::ranges::sort(parts);
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0U) { result += "-";
}
        result += parts[i];
    }
    return result;
}

}  // namespace engines
