#include "../kernel.h"

#include <cmath>
#include <algorithm>
#include <ranges>

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

}  // namespace quant

namespace quant {

}  // namespace quant
