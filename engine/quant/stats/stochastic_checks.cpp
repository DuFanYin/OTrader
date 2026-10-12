#include "../kernel.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace quant {

namespace {

constexpr int  ITO_SAMPLES     = 200'000;     // exact draws of W_t (antithetic pairs) for E[f(W_t)]
constexpr long ITO_PATH_BUDGET = 1'000'000;   // normal draws for the pathwise check

// f(W_s, s) with the derivatives Itô's formula needs: f_w, and the drift f_s + ½ f_ww.
struct ItoFn {
    int    code;
    double theta;

    double f(double w, double s) const {
        if (code == 1) return w * w - s;
        if (code == 2) return w * w * w;
        return std::exp(theta * w - 0.5 * theta * theta * s);
    }
    double fw(double w, double s) const {
        if (code == 1) return 2.0 * w;
        if (code == 2) return 3.0 * w * w;
        return theta * f(w, s);
    }
    double drift(double w) const {   // W² − s and the exponential martingale have none
        return code == 2 ? 3.0 * w : 0.0;
    }
    double target() const { return code == 0 ? 1.0 : 0.0; }
};

}  // namespace

ItoCheck ito_check(int function_code, double theta, double t, int n_steps) {
    const ItoFn fn{function_code, theta};
    const double tt = clamp_positive(t);
    std::mt19937_64 rng(19);
    std::normal_distribution<double> z(0.0, 1.0);
    ItoCheck out;
    out.target = fn.target();

    // E[f(W_t)]: W_t ~ N(0, t) exactly, so the steps play no part here.
    const int pairs = ITO_SAMPLES / 2;
    double mean = 0.0, m2 = 0.0;
    for (int k = 0; k < pairs; ++k) {
        const double w = std::sqrt(tt) * z(rng);
        const double y = 0.5 * (fn.f(w, tt) + fn.f(-w, tt));
        const double d = y - mean;
        mean += d / (k + 1);
        m2 += d * (y - mean);
    }
    out.value     = mean;
    out.std_error = std::sqrt(m2 / std::max(pairs - 1, 1) / pairs);

    // Itô's formula on each path: f(W_t, t) − f(0, 0) against Σ f_w ΔW + Σ (f_s + ½ f_ww) Δs, both from
    // the left end of each step. The gap shrinks like √Δs; it is reported relative to f(W_t, t) − f(0, 0).
    const int    n  = std::max(n_steps, 1);
    const double dt = tt / n, sq = std::sqrt(dt);
    const int    paths = static_cast<int>(std::clamp(ITO_PATH_BUDGET / n, 100L, 2000L));
    const double f0 = fn.f(0.0, 0.0);
    double gap2 = 0.0, move2 = 0.0;
    for (int p = 0; p < paths; ++p) {
        double w = 0.0, integral = 0.0;
        for (int i = 0; i < n; ++i) {
            const double dw = sq * z(rng);
            integral += fn.fw(w, i * dt) * dw + fn.drift(w) * dt;
            w += dw;
        }
        const double move = fn.f(w, tt) - f0;
        gap2  += (move - integral) * (move - integral);
        move2 += move * move;
    }
    out.residual = std::sqrt(gap2 / std::max(move2, 1e-300));
    out.paths    = paths;
    return out;
}

}  // namespace quant
