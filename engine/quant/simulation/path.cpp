#include "../kernel.h"

#include <cmath>
#include <algorithm>
#include <ranges>
#include <omp.h>

namespace quant {

GbmModel::GbmModel(const ModelParams& p) : p_(p) {}

double GbmModel::drift(double x, double /*t*/) const {
    return p_.mu * x;
}

double GbmModel::diffusion(double x, double /*t*/) const {
    return clamp_nonnegative(p_.sigma) * x;
}

double GbmModel::step(double x, double /*t*/, double dt, double z) const {
    const double dtt = clamp_positive(dt, 1e-12);
    const double s = clamp_nonnegative(p_.sigma);
    return x * std::exp((p_.mu - 0.5 * s * s) * dtt + s * std::sqrt(dtt) * z);
}

VasicekModel::VasicekModel(const ModelParams& p) : p_(p) {}

double VasicekModel::drift(double x, double /*t*/) const {
    return p_.kappa * (p_.theta - x);
}

double VasicekModel::diffusion(double /*x*/, double /*t*/) const {
    return clamp_nonnegative(p_.sigma);
}

double VasicekModel::step(double x, double t, double dt, double z) const {
    const double dtt = clamp_positive(dt, 1e-12);
    return x + drift(x, t) * dtt + diffusion(x, t) * std::sqrt(dtt) * z;
}

std::unique_ptr<StochasticModel> build_model(ModelType type, const ModelParams& params) {
    if (type == ModelType::Vasicek) {
        return std::make_unique<VasicekModel>(params);
    }
    return std::make_unique<GbmModel>(params);
}

double simulate_terminal_price_gbm(
    double spot,
    double rate,
    double vol,
    double maturity,
    std::mt19937_64& rng
) {
    std::normal_distribution<double> z(0.0, 1.0);
    ModelParams params{};
    params.mu = rate;
    params.sigma = vol;
    auto model = build_model(ModelType::GBM, params);
    return model->step(clamp_nonnegative(spot), 0.0, clamp_nonnegative(maturity), z(rng));
}

}  // namespace quant

namespace quant {

int simulation_path(
    int model_code,
    int n_steps,
    double dt,
    double sigma,
    double kappa,
    double theta,
    double x0,
    std::span<double> out_values
) {
    std::mt19937_64 rng(7);
    std::normal_distribution<double> z(0.0, 1.0);
    const int n = std::max(n_steps, 1);
    const double dtt = clamp_positive(dt);
    ModelParams params{};
    params.sigma = sigma;
    params.kappa = kappa;
    params.theta = theta;
    auto model = build_model(
        model_code == 1 ? ModelType::Vasicek : ModelType::GBM,
        params
    );

    if (out_values.size() < static_cast<size_t>(n + 1)) return 0;
    double x = x0;
    out_values[0] = x;
    for (int i = 0; i < n; ++i) {
        x = model->step(x, i * dtt, dtt, z(rng));
        out_values[i + 1] = x;
    }
    return n + 1;
}

int measure_density_path(
    double mu,
    double r,
    double sigma,
    double t,
    int n_steps,
    std::span<double> out_values
) {
    std::mt19937_64 rng(23);
    std::normal_distribution<double> z(0.0, 1.0);
    const int n = std::max(n_steps, 20);
    const double s = clamp_positive(sigma);
    const double tt = clamp_positive(t);
    const double dt = tt / static_cast<double>(n);
    const double theta_mpr = (mu - r) / s;
    if (out_values.size() < static_cast<size_t>(n + 1)) return 0;

    ModelParams p_params{};
    p_params.mu = mu;
    p_params.sigma = s;
    ModelParams q_params{};
    q_params.mu = r;
    q_params.sigma = s;
    auto p_model = build_model(ModelType::GBM, p_params);
    auto q_model = build_model(ModelType::GBM, q_params);

    double w = 0.0;
    double xp = 1.0;
    double xq = 1.0;
    out_values[0] = 1.0;
    for (int i = 0; i < n; ++i) {
        const double zi = z(rng);
        w += std::sqrt(dt) * zi;
        xp = p_model->step(xp, i * dt, dt, zi);
        xq = q_model->step(xq, i * dt, dt, zi);
        (void)xp;
        (void)xq;
        out_values[i + 1] = std::exp(-theta_mpr * w - 0.5 * theta_mpr * theta_mpr * ((i + 1) * dt));
    }
    return n + 1;
}

int measure_compare(
    double  mu,
    double  r,
    double  sigma,
    double  t,
    int     n_steps,
    int     n_paths,
    double  x0,
    std::span<double> out_stats_10,
    std::span<double> out_preview,
    int     preview_len
) {
    if (out_stats_10.size() < 10) return 0;
    const int    N       = std::max(n_steps, 2);
    const int    NP      = std::max(n_paths, 1);
    const double s       = clamp_positive(sigma, 1e-10);
    const double tt      = clamp_positive(t);
    const double dt      = tt / N;
    const int    PL      = std::min(preview_len, N);
    if (out_preview.size() < static_cast<size_t>(2 * PL)) return 0;

    std::mt19937_64 rng(42);
    std::normal_distribution<double> zdist(0.0, 1.0);
    ModelParams p_params{};
    p_params.mu = mu;
    p_params.sigma = s;
    ModelParams q_params{};
    q_params.mu = r;
    q_params.sigma = s;

    {
        auto p_model = build_model(ModelType::GBM, p_params);
        auto q_model = build_model(ModelType::GBM, q_params);
        double xp = x0, xq = x0;
        for (int i = 0; i < PL; ++i) {
            const double zi = zdist(rng);
            xp = p_model->step(xp, i * dt, dt, zi);
            xq = q_model->step(xq, i * dt, dt, zi);
            out_preview[i]      = xp;
            out_preview[PL + i] = xq;
        }
    }

    std::vector<double> fp(NP), fq(NP);

    #pragma omp parallel
    {
        std::mt19937_64 lrng(42 + omp_get_thread_num());
        std::normal_distribution<double> lz(0.0, 1.0);
        auto p_model = build_model(ModelType::GBM, p_params);
        auto q_model = build_model(ModelType::GBM, q_params);
        #pragma omp for schedule(static)
        for (int k = 0; k < NP; ++k) {
            double xp = x0, xq = x0;
            for (int i = 0; i < N; ++i) {
                const double zi = lz(lrng);
                xp = p_model->step(xp, i * dt, dt, zi);
                xq = q_model->step(xq, i * dt, dt, zi);
            }
            fp[k] = xp; fq[k] = xq;
        }
    }

    auto stats5 = [&](std::vector<double>& v, std::span<double> out) {
        std::ranges::sort(v);
        out[0] = sample_mean(v);
        out[1] = sample_variance(v);
        out[2] = interp_quantile_sorted(v, 0.05);
        out[3] = interp_quantile_sorted(v, 0.50);
        out[4] = interp_quantile_sorted(v, 0.95);
    };
    stats5(fp, out_stats_10.subspan<0, 5>());
    stats5(fq, out_stats_10.subspan<5, 5>());
    return PL;
}

}  // namespace quant
