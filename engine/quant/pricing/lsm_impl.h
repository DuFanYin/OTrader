#pragma once

// Executor-generic Longstaff-Schwartz core. The engine instantiates it with an
// OpenMP executor (lsm_american.cpp); a serial or a thread-pool executor runs the
// same code.
//
// Every parallel region is a loop over fixed-size path chunks, and each chunk
// owns its RNG stream and its partial sums, so the result is bit-identical for
// any executor and any thread count.
//
//   for_chunks(n, fn): call fn(i) for every i in [0, n), in any order, possibly
//                      concurrently; return only when all calls have finished.

#include "../kernel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace quant::lsm_detail {

constexpr int kChunk = 1024;   // paths per RNG stream / partial-sum block (even)

struct Output {
    double price     = 0.0;
    double std_error = 0.0;
    int    n_paths   = 0;
};

// Solve the 3x3 normal equations A c = y with partial pivoting.
inline bool solve3(std::array<std::array<double, 3>, 3> A, std::array<double, 3> y, std::array<double, 3>& c) {
    for (int col = 0; col < 3; ++col) {
        int piv = col;
        for (int r = col + 1; r < 3; ++r)
            if (std::abs(A[r][col]) > std::abs(A[piv][col])) piv = r;
        if (std::abs(A[piv][col]) < 1e-14) return false;
        std::swap(A[piv], A[col]);
        std::swap(y[piv], y[col]);
        for (int r = col + 1; r < 3; ++r) {
            const double f = A[r][col] / A[col][col];
            for (int k = col; k < 3; ++k) A[r][k] -= f * A[col][k];
            y[r] -= f * y[col];
        }
    }
    for (int r = 2; r >= 0; --r) {
        double acc = y[r];
        for (int k = r + 1; k < 3; ++k) acc -= A[r][k] * c[k];
        c[r] = acc / A[r][r];
    }
    return true;
}

template <class ForChunks>
Output run(double spot, double strike, double rate, double dividend_yield, double vol,
           double maturity, int n_paths, int n_steps, bool is_call,
           uint64_t seed, bool antithetic, ForChunks&& for_chunks) {
    Output out;
    if (spot <= 0.0 || strike <= 0.0 || vol <= 0.0 || maturity <= 0.0 || n_paths < 2 || n_steps < 1) {
        out.price = payoff(spot, strike, is_call);
        return out;
    }
    const int P = antithetic ? n_paths + (n_paths & 1) : n_paths;
    const int N = n_steps;
    const double dt    = maturity / N;
    const double drift = (rate - dividend_yield - 0.5 * vol * vol) * dt;
    const double diff  = vol * std::sqrt(dt);
    const int n_chunks = (P + kChunk - 1) / kChunk;
    const int stride   = antithetic ? 2 : 1;   // P and kChunk are even, so pairs never straddle chunks
    out.n_paths = P;
    const auto chunk_range = [P](int ch) { return std::pair{ch * kChunk, std::min(P, ch * kChunk + kChunk)}; };

    // Step-major path matrix: S[i * P + p].
    std::vector<double> S(static_cast<size_t>(N + 1) * P);
    for_chunks(n_chunks, [&](int ch) {
        std::mt19937_64 rng(seed + 0x9E3779B97F4A7C15ull * static_cast<uint64_t>(ch + 1));
        std::normal_distribution<double> nd(0.0, 1.0);
        const auto [p0, p1] = chunk_range(ch);
        for (int p = p0; p < p1; ++p) S[p] = spot;
        for (int i = 1; i <= N; ++i) {
            double*       cur  = &S[static_cast<size_t>(i) * P];
            const double* prev = &S[static_cast<size_t>(i - 1) * P];
            for (int p = p0; p < p1; p += stride) {
                const double z = nd(rng);
                cur[p] = prev[p] * std::exp(drift + diff * z);
                if (antithetic) cur[p + 1] = prev[p + 1] * std::exp(drift - diff * z);
            }
        }
    });

    std::vector<double> disc_pow(N + 1);
    for (int k = 0; k <= N; ++k) disc_pow[k] = std::exp(-rate * dt * k);

    std::vector<double> cf(P);
    std::vector<int>    tex(P, N);
    for (int p = 0; p < P; ++p) cf[p] = payoff(S[static_cast<size_t>(N) * P + p], strike, is_call);

    // Regression sums over in-the-money paths: n, x, x^2, x^3, x^4, y, xy, x^2 y.
    std::vector<std::array<double, 8>> part(n_chunks);
    for (int i = N - 1; i >= 1; --i) {
        const double* Si = &S[static_cast<size_t>(i) * P];
        for_chunks(n_chunks, [&](int ch) {
            std::array<double, 8> acc{};
            const auto [p0, p1] = chunk_range(ch);
            for (int p = p0; p < p1; ++p) {
                if (payoff(Si[p], strike, is_call) <= 0.0) continue;
                const double x = Si[p] / strike, x2 = x * x;
                const double y = cf[p] * disc_pow[tex[p] - i];
                acc[0] += 1.0; acc[1] += x; acc[2] += x2; acc[3] += x2 * x; acc[4] += x2 * x2;
                acc[5] += y;   acc[6] += x * y; acc[7] += x2 * y;
            }
            part[ch] = acc;
        });
        std::array<double, 8> t{};
        for (const auto& a : part) for (int k = 0; k < 8; ++k) t[k] += a[k];   // fixed order
        if (t[0] < 3.0) continue;

        std::array<double, 3> coef{};
        const std::array<std::array<double, 3>, 3> A{{{t[0], t[1], t[2]}, {t[1], t[2], t[3]}, {t[2], t[3], t[4]}}};
        if (!solve3(A, {t[5], t[6], t[7]}, coef)) continue;

        for_chunks(n_chunks, [&](int ch) {
            const auto [p0, p1] = chunk_range(ch);
            for (int p = p0; p < p1; ++p) {
                const double ex = payoff(Si[p], strike, is_call);
                if (ex <= 0.0) continue;
                const double x = Si[p] / strike;
                if (ex > coef[0] + coef[1] * x + coef[2] * x * x) { cf[p] = ex; tex[p] = i; }
            }
        });
    }

    // Discounted cash flows; antithetic pairs are averaged before the standard error.
    const int n_obs = P / stride;
    std::vector<std::array<double, 2>> ps(n_chunks);
    for_chunks(n_chunks, [&](int ch) {
        double s1 = 0.0, s2 = 0.0;
        const auto [p0, p1] = chunk_range(ch);
        for (int p = p0; p < p1; p += stride) {
            double v = cf[p] * disc_pow[tex[p]];
            if (antithetic) v = 0.5 * (v + cf[p + 1] * disc_pow[tex[p + 1]]);
            s1 += v; s2 += v * v;
        }
        ps[ch] = {s1, s2};
    });
    double s1 = 0.0, s2 = 0.0;
    for (const auto& a : ps) { s1 += a[0]; s2 += a[1]; }
    const double mean = s1 / n_obs;
    const double var  = std::max(s2 / n_obs - mean * mean, 0.0) * n_obs / std::max(n_obs - 1, 1);
    out.price     = std::max(mean, payoff(spot, strike, is_call));
    out.std_error = std::sqrt(var / n_obs);
    return out;
}

}  // namespace quant::lsm_detail
