#pragma once

// Minimal C API declarations used by backtest data engine (LetsBeRational, thirdparty/).
// Implied volatility of an undiscounted Black price; returns -DBL_MAX below intrinsic and
// DBL_MAX above the maximum attainable price.
extern "C" double ImpliedBlackVolatility(double price, double F, double K, double T,
                                         double q /* q=+1 call, -1 put */);
