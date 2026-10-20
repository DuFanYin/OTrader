#include "../kernel.h"
#include "quant/optimizer/nelder_mead.h"

#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <span>
#include <vector>

namespace quant {

namespace {

// Heston (1993) characteristic function phi(u) = E[exp(i*u * ln S_T)].
// v0 current variance, kappa mean reversion, theta long-run variance, xi vol-of-vol, rho correlation.
std::complex<double> heston_cf(
    std::complex<double> u,
    double spot, double rate, double maturity,
    double v0, double kappa, double theta, double xi, double rho
) {
    using C = std::complex<double>;
    const C i(0.0, 1.0);
    const double T = maturity;
    const double x = std::log(spot);

    // d = sqrt( (rho*xi*i*u - kappa)^2 + xi^2*(i*u + u^2) )
    const C b      = kappa - rho * xi * i * u;
    const C disc2  = b * b + xi * xi * (i * u + u * u);
    // Take sqrt with positive real part for stability
    C d = std::sqrt(disc2);
    if (std::real(d) < 0.0) d = -d;

    const C g   = (b - d) / (b + d);
    const C edt = std::exp(-d * T);

    const C frac = (1.0 - g * edt) / (1.0 - g);
    const C logfrac = std::log(frac);

    const C A = i * u * x
              + i * u * rate * T
              + (kappa * theta / (xi * xi))
                * ((b - d) * T - 2.0 * logfrac);

    const C B = ((b - d) / (xi * xi))
              * (1.0 - edt) / (1.0 - g * edt);

    return std::exp(A + B * v0);
}

// Compute one probability P_j via the Gil-Pelaez inversion formula.
// j=1: use CF at u - i (P1, "delta" probability)
// j=2: use CF at u    (P2, risk-neutral probability)
// Integral: (1/pi) * integral_0^inf Re[ exp(-i*u*ln(K)) * phi_j(u) / (i*u) ] du
double heston_P(
    double ln_K, double spot, double rate, double maturity,
    double v0, double kappa, double theta, double xi, double rho,
    int j
) {
    using C = std::complex<double>;
    const C ic(0.0, 1.0);

    const int    M  = 512;
    const double lo = 1e-5;
    const double hi = 100.0;
    const double h  = (hi - lo) / M;

    auto integrand = [&](double u_real) -> double {
        C phi_j;
        if (j == 1) {
            // phi_1 uses modified argument: standard trick phi_1(u) = phi(u-i) / phi(-i)
            // phi(-i) = E[S_T] = S*exp(r*T) so we normalize:
            const C u_mod = C(u_real, -1.0);
            const C cf_mod = heston_cf(u_mod, spot, rate, maturity, v0, kappa, theta, xi, rho);
            const double forward = spot * std::exp(rate * maturity);
            phi_j = cf_mod / forward;
        } else {
            phi_j = heston_cf(C(u_real, 0.0), spot, rate, maturity, v0, kappa, theta, xi, rho);
        }
        const C num = std::exp(-ic * u_real * ln_K) * phi_j;
        const C denom = ic * C(u_real, 0.0);
        if (std::abs(denom) < 1e-15) return 0.0;
        return std::real(num / denom);
    };

    double s = integrand(lo) + integrand(hi);
    for (int k = 1; k < M; k += 2) s += 4.0 * integrand(lo + k * h);
    for (int k = 2; k < M; k += 2) s += 2.0 * integrand(lo + k * h);
    s *= h / 3.0;
    return 0.5 + s / std::numbers::pi;
}

}  // namespace

double heston_call_price(double spot, double strike, double rate, double maturity,
                         const HestonParams& h) {
    const auto [v0, kappa, theta, xi, rho] = h;
    if (spot <= 0.0 || strike <= 0.0 || maturity <= 0.0 || v0 <= 0.0)
        return std::max(spot - strike, 0.0);

    const double disc  = std::exp(-rate * maturity);
    const double P1 = heston_P(std::log(strike), spot, rate, maturity, v0, kappa, theta, xi, rho, 1);
    const double P2 = heston_P(std::log(strike), spot, rate, maturity, v0, kappa, theta, xi, rho, 2);

    // Heston (1993) Eq.(10): C = S*P1 - K*e^{-rT}*P2
    const double price = spot * P1 - strike * disc * P2;
    return std::max(price, 0.0);
}

HestonFit heston_calibrate(
    double spot, double rate,
    std::span<const double> strikes,
    std::span<const double> maturities,
    std::span<const double> call_prices,
    const HestonParams& init, int max_iter
) {
    HestonFit fit;
    fit.params = init;
    const int n = static_cast<int>(call_prices.size());
    if (n == 0) return fit;

    auto objective = [&](std::span<const double> x) -> double {
        double sse = 0.0;
        for (int i = 0; i < n; ++i) {
            const double err = heston_call_price(spot, strikes[i], rate, maturities[i],
                                                 {x[0], x[1], x[2], x[3], x[4]})
                               - call_prices[i];
            sse += err * err;
        }
        return std::isfinite(sse) ? sse / n : 1e300; // a characteristic function that overflowed
    };

    const std::array<double, 5> x0    = {init.v0, init.kappa, init.theta, init.xi, init.rho};
    const std::array<double, 5> lower = {1e-4, 1e-3, 1e-4, 1e-4, -0.999};
    const std::array<double, 5> upper = {2.0,  20.0,  2.0,  3.0,  0.999};
    std::array<double, 5> step{};  // a first step of 15% of each, 0.1 for one near zero
    for (std::size_t i = 0; i < step.size(); ++i)
        step[i] = std::abs(x0[i]) > 1e-8 ? 0.15 * x0[i] : 0.1;
    const auto opt = minimise([&](const std::array<double, 5>& x) { return objective(x); }, x0,
                              step, lower, upper, 1e-8, max_iter);
    fit.params     = {opt.x[0], opt.x[1], opt.x[2], opt.x[3], opt.x[4]};
    fit.iterations = opt.iterations;
    fit.converged  = opt.converged;
    const double x[] = {fit.params.v0, fit.params.kappa, fit.params.theta, fit.params.xi,
                        fit.params.rho};
    fit.rmse = std::sqrt(objective(x));
    return fit;
}

}  // namespace quant
