#pragma once

/**
 * An option at its expiry, the one set of rules for every book that holds one: what it settles at
 * (the engine's bookkeeping, the simulated account) and the worst a set of them can be worth then
 * (a holding's max loss, the risk check's margin).
 */

#include "model/constant.hpp"
#include "model/symbol.hpp"
#include <span>

namespace model {

/** OCC exercises an option in the money by this much or more at expiry. */
inline constexpr double kExerciseThreshold = 0.01;

/** What the option is worth at expiry with the underlying at `price`, to the cent. */
double intrinsic(const OptionId& id, double price);

/** How one contract of the option settles at expiry with the underlying at `price`. */
struct ExpirySettlement {
    double close = 0.0;  // the option closes at this: cash-settled its intrinsic value, else 0
    double shares = 0.0; // share-settled in the money, the shares one contract bought delivers at
                         // the strike: bought +, sold -
};
ExpirySettlement settle_at_expiry(const OptionId& id, double price, bool physical,
                                  double multiplier);

/** An option of one expiry in a book: units are contracts times the multiplier, bought +. */
struct ExpiryLeg {
    OptionType type = OptionType::CALL;
    double strike = 0.0;
    double units = 0.0;
};

/** What legs of one expiry are worth at it, at the worst: at 0 or a strike. calls_net is the
 * slope past the last strike: below 0, the value falls without bound as the underlying rises. */
struct ExpiryWorst {
    double worst = 0.0;
    double calls_net = 0.0;
};
ExpiryWorst worst_at_expiry(std::span<const ExpiryLeg> legs);

} // namespace model
