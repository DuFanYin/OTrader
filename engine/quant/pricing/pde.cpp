#include "../kernel.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <vector>

namespace quant {

namespace {

double intrinsic(double s, double k, bool is_call) {
    return is_call ? std::max(s - k, 0.0) : std::max(k - s, 0.0);
}

// Thomas algorithm for a[i] x[i-1] + b[i] x[i] + c[i] x[i+1] = d[i] (a[0], c[n-1] unused).
bool solve_tridiagonal(const std::vector<double>& a, const std::vector<double>& b,
                       const std::vector<double>& c, const std::vector<double>& d,
                       std::vector<double>& cp, std::vector<double>& dp, std::vector<double>& x) {
    const int n = static_cast<int>(b.size());
    if (std::abs(b[0]) < 1e-300) return false;
    cp[0] = c[0] / b[0];
    dp[0] = d[0] / b[0];
    for (int i = 1; i < n; ++i) {
        const double denom = b[i] - a[i] * cp[i - 1];
        if (std::abs(denom) < 1e-300) return false;
        cp[i] = i < n - 1 ? c[i] / denom : 0.0;
        dp[i] = (d[i] - a[i] * dp[i - 1]) / denom;
    }
    x[n - 1] = dp[n - 1];
    for (int i = n - 2; i >= 0; --i) x[i] = dp[i] - cp[i] * x[i + 1];
    return true;
}

}  // namespace

// Finite-difference solver on S in [0, s_max_mult * max(K, S0)], marching in
// time-to-expiry tau. Scheme and American (PSOR) handling follow QF-205
// (src/option_calculator/methods/fd.py) with three corrections:
//   - boundary values use the tau of the step being solved, (n+1)·dt
//     (QF-205 used T - n·dt, i.e. the time-0 discount on the first step);
//   - American boundaries are floored at intrinsic value (S=0 put = K);
//   - PSOR does not count the boundary term twice at the first / last node.
PdeOutcome pde_solve(const PdeSpec& p) {
    PdeOutcome out;
    const int    M     = std::max(p.s_steps, 4);
    const double sigma = clamp_positive(p.vol);
    const double T     = clamp_positive(p.maturity);
    const double r     = p.rate;
    const double b_    = p.rate - p.dividend_yield;            // drift
    const double s_max = std::max(p.s_max_mult * std::max(p.strike, p.spot), 1e-8);
    const double ds    = s_max / M;
    const double theta = p.method == 1 ? 1.0 : (p.method == 2 ? 0.0 : 0.5);

    int N = std::max(p.t_steps, 4);
    if (p.method == 2) {
        // Explicit stability: 1 - dt (sigma^2 i^2 + r) >= 0 for every interior node.
        const double i_max = M - 1;
        const double need  = T * (sigma * sigma * i_max * i_max + std::max(r, 0.0));
        if (N < need) {
            N = std::min(static_cast<int>(std::ceil(need * 1.01)), kPdeMaxExplicitSteps);
            out.refined = true;
        }
    }
    out.t_steps_used = N;
    const double dt = T / N;

    std::vector<double> s(M + 1), v(M + 1), intr(M + 1);
    for (int i = 0; i <= M; ++i) {
        s[i]    = i * ds;
        intr[i] = intrinsic(s[i], p.strike, p.is_call);
        v[i]    = intr[i];
    }

    const int m = M - 1;   // interior nodes 1..M-1 -> rows 0..m-1
    std::vector<double> alpha(m), beta(m), gamma(m);
    for (int k = 0; k < m; ++k) {
        const double i = k + 1;
        alpha[k] = 0.5 * dt * (sigma * sigma * i * i - b_ * i);
        beta[k]  = dt * (sigma * sigma * i * i + r);
        gamma[k] = 0.5 * dt * (sigma * sigma * i * i + b_ * i);
    }
    std::vector<double> a(m), bb(m), c(m), d(m), cp(m), dp(m), x(m), v_new(M + 1);
    for (int k = 0; k < m; ++k) {
        a[k]  = -theta * alpha[k];
        bb[k] = 1.0 + theta * beta[k];
        c[k]  = -theta * gamma[k];
    }

    for (int n = 0; n < N; ++n) {
        const double tau = (n + 1) * dt;
        double v0, vmax;
        if (p.is_call) {
            v0   = 0.0;
            vmax = std::max(s_max * std::exp(-p.dividend_yield * tau) - p.strike * std::exp(-r * tau), 0.0);
        } else {
            v0   = p.strike * std::exp(-r * tau);
            vmax = 0.0;
        }
        if (p.american) {
            v0   = std::max(v0, intr[0]);
            vmax = std::max(vmax, intr[M]);
        }

        if (p.method == 2) {
            for (int k = 0; k < m; ++k) {
                const int i = k + 1;
                double val = alpha[k] * v[i - 1] + (1.0 - beta[k]) * v[i] + gamma[k] * v[i + 1];
                if (p.american) val = std::max(val, intr[i]);
                v_new[i] = val;
            }
        } else {
            for (int k = 0; k < m; ++k) {
                const int i = k + 1;
                d[k] = (1.0 - theta) * alpha[k] * v[i - 1] + (1.0 - (1.0 - theta) * beta[k]) * v[i]
                     + (1.0 - theta) * gamma[k] * v[i + 1];
            }
            d[0]     -= a[0] * v0;       // known boundary values moved to the right-hand side
            d[m - 1] -= c[m - 1] * vmax;

            if (!p.american) {
                if (!solve_tridiagonal(a, bb, c, d, cp, dp, x)) { out.price = 0.0; return out; }
            } else {
                for (int k = 0; k < m; ++k) x[k] = v[k + 1];
                for (int it = 0; it < p.psor_max_iter; ++it) {
                    double max_change = 0.0;
                    for (int k = 0; k < m; ++k) {
                        const double left  = k > 0     ? a[k] * x[k - 1] : 0.0;
                        const double right = k < m - 1 ? c[k] * x[k + 1] : 0.0;
                        const double gs    = (d[k] - left - right) / bb[k];
                        const double next  = std::max(intr[k + 1], x[k] + p.psor_omega * (gs - x[k]));
                        max_change = std::max(max_change, std::abs(next - x[k]));
                        x[k] = next;
                    }
                    ++out.psor_iterations;
                    if (max_change < p.psor_tol) break;
                }
            }
            for (int k = 0; k < m; ++k) v_new[k + 1] = x[k];
        }
        v_new[0] = v0;
        v_new[M] = vmax;
        v.swap(v_new);
    }

    // Linear interpolation at spot (np.interp in QF-205).
    const double pos = p.spot / ds;
    const int    si  = std::min(std::max(static_cast<int>(pos), 0), M - 1);
    const double w   = pos - si;
    out.price = v[si] * (1.0 - w) + v[si + 1] * w;
    if (!std::isfinite(out.price)) out.price = 0.0;
    return out;
}

// Backward-compatible entry point: European call, method 0 = Crank-Nicolson, 1 = implicit.
double pde_price(
    double spot, double strike, double rate, double vol, double maturity,
    double dividend_yield, int s_steps, int t_steps, int method
) {
    PdeSpec spec;
    spec.spot = spot; spec.strike = strike; spec.rate = rate; spec.dividend_yield = dividend_yield;
    spec.vol = vol; spec.maturity = maturity; spec.s_steps = s_steps; spec.t_steps = t_steps;
    spec.method = method;
    return pde_solve(spec).price;
}

double vol_surface_interp(
    std::span<const double> strikes,
    std::span<const double> expiries,
    std::span<const double> ivs,
    double spot, double target_strike, double target_expiry
) {
    const int n_points = static_cast<int>(strikes.size());
    if (expiries.size() != strikes.size() || ivs.size() != strikes.size() || n_points < 1 || spot <= 0.0
            || target_strike <= 0.0 || target_expiry <= 0.0)
        return -1.0;

    const double tgt_m = std::log(target_strike / spot);
    const double tgt_v = std::sqrt(target_expiry);
    std::vector<double> ms(n_points), vs(n_points);
    for (int i = 0; i < n_points; ++i) {
        ms[i] = std::log(strikes[i] / spot);
        vs[i] = std::sqrt(clamp_positive(expiries[i]));
    }
    std::vector<double> um = ms, uv = vs;
    std::ranges::sort(um);
    um.erase(std::ranges::unique(um).begin(), um.end());
    std::ranges::sort(uv);
    uv.erase(std::ranges::unique(uv).begin(), uv.end());

    if (um.size() < 2 || uv.size() < 2) {
        int best = 0; double best_d2 = 1e30;
        for (int i = 0; i < n_points; ++i) {
            const double dm = ms[i] - tgt_m, dv = vs[i] - tgt_v;
            const double d2 = dm * dm + dv * dv;
            if (d2 < best_d2) { best_d2 = d2; best = i; }
        }
        return ivs[best];
    }

    auto bracket = [](const std::vector<double>& axis, double t) -> std::pair<int, int> {
        if (t <= axis.front()) return {0, 0};
        if (t >= axis.back())  return {(int)axis.size() - 1, (int)axis.size() - 1};
        for (int i = 0; i < (int)axis.size() - 1; ++i)
            if (axis[i] <= t && t <= axis[i + 1]) return {i, i + 1};
        return {(int)axis.size() - 1, (int)axis.size() - 1};
    };
    auto [mi0, mi1] = bracket(um, tgt_m);
    auto [vi0, vi1] = bracket(uv, tgt_v);
    const double m0 = um[mi0], m1 = um[mi1];
    const double v0 = uv[vi0], v1 = uv[vi1];
    const double wm = (m1 > m0) ? (tgt_m - m0) / (m1 - m0) : 0.0;
    const double wv = (v1 > v0) ? (tgt_v - v0) / (v1 - v0) : 0.0;

    auto corner_iv = [&](double cm, double cv) -> double {
        int best = 0; double best_d = 1e30;
        for (int i = 0; i < n_points; ++i) {
            const double dm = ms[i] - cm, dv = vs[i] - cv;
            const double d = dm * dm + dv * dv;
            if (d < best_d) { best_d = d; best = i; }
        }
        return ivs[best];
    };

    const double q00 = corner_iv(m0, v0), q10 = corner_iv(m1, v0);
    const double q01 = corner_iv(m0, v1), q11 = corner_iv(m1, v1);
    const double interp = q00*(1-wm)*(1-wv) + q10*wm*(1-wv) + q01*(1-wm)*wv + q11*wm*wv;
    return std::isfinite(interp) ? interp : -1.0;
}

}  // namespace quant
