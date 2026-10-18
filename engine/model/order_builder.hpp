#pragma once

/** Pure functions that build orders: a combo's legs from its type and roles, a leg from an
 * option, an order's name, and the order request itself. The caller logs. */

#include "model/constant.hpp"
#include "model/object.hpp"
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace model {

using ComboGetContractFn = std::function<const Contract*(const std::string&)>;

/** Single entry: build legs + signature by combo_type over its roles' options (by symbol). Legs
 * are sized per combo unit (Leg.ratio); the order's quantity multiplies them. */
std::pair<std::vector<Leg>, std::string>
build_combo(const std::unordered_map<std::string, std::string>& roles, ComboType combo_type,
            Side side, const ComboGetContractFn& find_contract);

/** Create one leg of the option named `symbol`; use when building custom combos outside
 * build_combo. `ratio` is the leg's size per combo unit. */
Leg create_leg(const std::string& symbol, Side side, int ratio,
               const ComboGetContractFn& find_contract, std::optional<double> price = std::nullopt);

/** A combo's signature: its option legs' symbols, sorted, joined by '|'. */
std::string generate_combo_signature(std::span<const Leg> legs);

/** A leg of one contract: its side, ratio 1. */
Leg contract_leg(const Contract& contract, Side side);
/** Whole units of `legs`, each with its side and ratio. One leg at ratio 1 is its contract's own
 * order, named by it; anything else is a combo of combo_type, named <combo_type>_<signature>. A
 * limit price is the net a unit (paid positive, received negative), to the cent; a market order's
 * is none. */
OrderRequest order_request(std::span<const Leg> legs, double price, double quantity,
                           OrderType order_type, ComboType combo_type = ComboType::SINGLE_LEG);
/** One order at market closing the options, each against its own side, in the smallest whole
 * units of their sizes: one option alone as its own order, several as a combo. Why not when none
 * is open or a contract is not found. */
std::expected<OrderRequest, std::string>
closing_order(std::span<const OptionPosition> options, const ComboGetContractFn& find_contract);

} // namespace model
