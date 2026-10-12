// Longstaff-Schwartz least-squares Monte Carlo for American options.
// Reworked from Option-Pricing (monteCarlo/monteCarlo*.cpp); the algorithm lives
// in lsm_impl.h so bench/concurrency can run the same code under other executors.

#include "../kernel.h"
#include "lsm_impl.h"

#include <omp.h>

namespace quant {

LsmResult lsm_american_price(double spot, double strike, double rate, double dividend_yield, double vol,
                             double maturity, int n_paths, int n_steps, bool is_call,
                             uint64_t seed, bool antithetic) {
    const auto omp_for_chunks = [](int n, auto&& fn) {
#pragma omp parallel for schedule(static)
        for (int i = 0; i < n; ++i) fn(i);
    };
    const auto o = lsm_detail::run(spot, strike, rate, dividend_yield, vol, maturity, n_paths, n_steps,
                                   is_call, seed, antithetic, omp_for_chunks);
    return {o.price, o.std_error, o.n_paths};
}

}  // namespace quant
