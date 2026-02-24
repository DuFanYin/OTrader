#pragma once

/**
 * Shared ComboBuilder (engines_cpp + backtest_cpp).
 * Pure function style: get_contract from caller; logs output as LogIntent (vector<LogData>*).
 */

#include "../utilities/constant.hpp"
#include "../utilities/object.hpp"
#include "../utilities/portfolio.hpp"
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engines {

using ComboGetContractFn = std::function<const utilities::ContractData*(const std::string&)>;

class ComboBuilderEngine {
public:
    ComboBuilderEngine() = default;

    /** Create one leg; get_contract required when not called from combo_builder (which sets current_). */
    utilities::Leg create_leg(const utilities::OptionData& option, utilities::Direction direction,
        int volume, std::optional<double> price = std::nullopt,
        ComboGetContractFn get_contract_fn = nullptr);

    /** Build combo legs and signature; caller passes get_contract. Optional out_logs: append LogIntent for caller to put_intent_log. */
    std::pair<std::vector<utilities::Leg>, std::string> combo_builder(
        const std::unordered_map<std::string, utilities::OptionData*>& option_data,
        utilities::ComboType combo_type, utilities::Direction direction, int volume,
        ComboGetContractFn get_contract_fn, std::vector<utilities::LogData>* out_logs = nullptr);

    // Combo builders (same names and logic as Python)
    std::pair<std::vector<utilities::Leg>, std::string> straddle(
        const std::unordered_map<std::string, utilities::OptionData*>& option_data,
        utilities::Direction direction, int volume);
    std::pair<std::vector<utilities::Leg>, std::string> strangle(
        const std::unordered_map<std::string, utilities::OptionData*>& option_data,
        utilities::Direction direction, int volume);
    std::pair<std::vector<utilities::Leg>, std::string> iron_condor(
        const std::unordered_map<std::string, utilities::OptionData*>& option_data,
        utilities::Direction direction, int volume);
    std::pair<std::vector<utilities::Leg>, std::string> risk_reversal(
        const std::unordered_map<std::string, utilities::OptionData*>& option_data,
        utilities::Direction direction, int volume);
    std::pair<std::vector<utilities::Leg>, std::string> custom(
        const std::unordered_map<std::string, utilities::OptionData*>& option_data,
        utilities::Direction direction, int volume);
    std::pair<std::vector<utilities::Leg>, std::string> spread(
        const std::unordered_map<std::string, utilities::OptionData*>& option_data,
        utilities::Direction direction, int volume);
    std::pair<std::vector<utilities::Leg>, std::string> diagonal_spread(
        const std::unordered_map<std::string, utilities::OptionData*>& option_data,
        utilities::Direction direction, int volume);
    std::pair<std::vector<utilities::Leg>, std::string> ratio_spread(
        const std::unordered_map<std::string, utilities::OptionData*>& option_data,
        utilities::Direction direction, int volume);
    std::pair<std::vector<utilities::Leg>, std::string> butterfly(
        const std::unordered_map<std::string, utilities::OptionData*>& option_data,
        utilities::Direction direction, int volume);
    std::pair<std::vector<utilities::Leg>, std::string> inverse_butterfly(
        const std::unordered_map<std::string, utilities::OptionData*>& option_data,
        utilities::Direction direction, int volume);
    std::pair<std::vector<utilities::Leg>, std::string> iron_butterfly(
        const std::unordered_map<std::string, utilities::OptionData*>& option_data,
        utilities::Direction direction, int volume);
    std::pair<std::vector<utilities::Leg>, std::string> condor(
        const std::unordered_map<std::string, utilities::OptionData*>& option_data,
        utilities::Direction direction, int volume);
    std::pair<std::vector<utilities::Leg>, std::string> box_spread(
        const std::unordered_map<std::string, utilities::OptionData*>& option_data,
        utilities::Direction direction, int volume);

    static std::string generate_combo_signature(const std::vector<utilities::Leg>& legs);

private:
    std::pair<std::vector<utilities::Leg>, std::string> combo_builder_impl(
        const std::unordered_map<std::string, utilities::OptionData*>& option_data,
        utilities::ComboType combo_type, utilities::Direction direction, int volume);

    ComboGetContractFn current_get_contract_;
    std::vector<utilities::LogData>* current_out_logs_ = nullptr;
};

}  // namespace engines
