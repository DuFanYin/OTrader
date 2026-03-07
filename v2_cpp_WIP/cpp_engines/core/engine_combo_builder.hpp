#pragma once

/** ComboBuilder: leg construction; get_contract from caller. */

#include "../utilities/base_engine.hpp"
#include "../utilities/constant.hpp"
#include "../utilities/object.hpp"
#include "../utilities/portfolio.hpp"
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engines {

using ComboGetContractFn = std::function<const utilities::ContractData*(const std::string&)>;

class ComboBuilderEngine : public utilities::BaseEngine {
public:
    ComboBuilderEngine() = default;
    explicit ComboBuilderEngine(utilities::MainEngine* main)
        : BaseEngine(main, "ComboBuilder") {}

    /** Create one leg; get_contract when not from combo_builder. */
    utilities::Leg create_leg(const utilities::OptionData& option, utilities::Direction direction,
        int volume, std::optional<double> price = std::nullopt,
        const ComboGetContractFn& get_contract_fn = nullptr);

    /** Build combo legs + signature; optional out_logs. */
    std::pair<std::vector<utilities::Leg>, std::string> combo_builder(
        const std::unordered_map<std::string, utilities::OptionData*>& option_data,
        utilities::ComboType combo_type, utilities::Direction direction, int volume,
        ComboGetContractFn get_contract_fn, std::vector<utilities::LogData>* out_logs = nullptr);

    // Combo builders
    std::pair<std::vector<utilities::Leg>, std::string> single_leg(
        const std::unordered_map<std::string, utilities::OptionData*>& option_data,
        utilities::Direction direction, int volume);
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

    static std::string generate_combo_signature(std::span<const utilities::Leg> legs);

private:
    std::pair<std::vector<utilities::Leg>, std::string> combo_builder_impl(
        const std::unordered_map<std::string, utilities::OptionData*>& option_data,
        utilities::ComboType combo_type, utilities::Direction direction, int volume);

    ComboGetContractFn current_get_contract_;
    std::vector<utilities::LogData>* current_out_logs_ = nullptr;
};

}  // namespace engines
