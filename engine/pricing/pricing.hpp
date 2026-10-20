#pragma once

/** Pricing: a frame's quotes made into its forwards, IVs, Greeks and smiles, the same code live and
 * in a backtest. */

#include "model/object.hpp"
#include <chrono>
#include <cstdint>
#include <span>
#include <string_view>

namespace utilities {
class ThreadPool;
}

namespace pricing {

/** Live pricing's time_step, and the time left under which every step counts. */
inline constexpr std::chrono::seconds kLiveTimeStep{10};
inline constexpr std::chrono::hours kTimeStepMinTimeLeft{1};

/** Each expiry's rate (rates at its time), each chain's forward by put-call parity, then each
 * option's IV and Greeks as Black on them; on `pool` when given (any thread), else here. With
 * quotes_changed (one per option, on a snapshot priced before) only what moved is priced again; a
 * move of the underlying alone rescales delta and gamma. time_step (live): an expiry's time counts
 * as moved once per step while more than kTimeStepMinTimeLeft is left. iv_price_mode: one of
 * model::iv_price_mode_of's. */
void price_snapshot(model::PortfolioSnapshot& snapshot, const model::RateCurve& rates,
                    std::string_view iv_price_mode,
                    std::span<const std::uint8_t> quotes_changed = {},
                    bool underlying_changed = true,
                    std::chrono::seconds time_step = std::chrono::seconds::zero(),
                    utilities::ThreadPool* pool = nullptr);

/** Live fits' step: a chain's smile is fitted again once its last fit is this much older. */
inline constexpr std::chrono::seconds kLiveFitStep{60};

/** Each chain's smile (model::Frame::smile) fitted to its options' IVs (quant::svi_fit) at its
 * forward and time as priced: per strike the option out of the money (the put below the forward,
 * the call at or above it), quoted both ways with a spread at most half its mid, each residual over its
 * half spread in total variance (by its vega). With a step (live), a chain tried less than `step`
 * before (by its time left) keeps its smile; zero: every chain fitted. On `pool` when given (any
 * thread), else here. The snapshot priced (price_snapshot). */
void fit_smiles(model::PortfolioSnapshot& snapshot,
                std::chrono::seconds step = std::chrono::seconds::zero(),
                utilities::ThreadPool* pool = nullptr);

/** What price_snapshot and fit_smiles make of a fixed set of quotes, every value's bits hashed: the
 * same as long as pricing a stored day would give the same values. */
std::uint64_t fingerprint();

} // namespace pricing
