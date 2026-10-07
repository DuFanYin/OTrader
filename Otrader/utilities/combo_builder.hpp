#pragma once

/**
 * Combo builder helper: build option combo legs + signature from option_data and type.
 * No engine lifecycle; pure functions. Logging is the caller's responsibility.
 */

#include "constant.hpp"
#include "object.hpp"
#include "portfolio.hpp"
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace utilities {
namespace combo {

using ComboGetContractFn = std::function<const ContractData*(const std::string&)>;

struct ComboBuildOptions {
    ComboGetContractFn get_contract;
};

/** Single entry: build legs + signature by combo_type. Legs are sized per combo unit
 * (Leg.ratio); the order's volume multiplies them. */
std::pair<std::vector<Leg>, std::string> build_combo(
    const std::unordered_map<std::string, OptionData*>& option_data,
    ComboType combo_type, Direction direction,
    const ComboBuildOptions& options = {});

/** Legs that flatten a held multi-leg position: each leg against its own held side (so mixed
 * long/short combos unwind correctly), sized per unit of position.quantity. option_data is keyed
 * by leg symbol. Send as a CUSTOM combo with volume |position.quantity|. */
std::pair<std::vector<Leg>, std::string> build_closing_combo(
    const OptionPositionData& position,
    const std::unordered_map<std::string, OptionData*>& option_data,
    const ComboBuildOptions& options = {});

/** Create one leg from option; use when building custom combos outside build_combo.
 * `ratio` is the leg's size per combo unit. */
Leg create_leg(const OptionData& option, Direction direction, int ratio,
               const ComboGetContractFn& get_contract,
               std::optional<double> price = std::nullopt);

/** Generate signature string from legs (symbol-derived). */
std::string generate_combo_signature(std::span<const Leg> legs);

} // namespace combo
} // namespace utilities
