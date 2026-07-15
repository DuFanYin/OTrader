#pragma once

#include "../thirdparty/lets_be_rational/src/importexport.h"

// Minimal C API declarations used by backtest data engine.
EXPORT_EXTERN_C double implied_volatility_from_a_transformed_rational_guess(
    double price, double F, double K, double T, double q /* q=+1 call, -1 put */
);
