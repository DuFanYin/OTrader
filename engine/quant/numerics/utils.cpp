#include "../kernel.h"

#include <cmath>

namespace quant {

double clamp_positive(double x, double eps) {
    return x < eps ? eps : x;
}

double clamp_nonnegative(double x) {
    return x < 0.0 ? 0.0 : x;
}

double norm_cdf(double x) {
    return 0.5 * (1.0 + std::erf(x * 0.70710678118654752440));
}

double norm_pdf(double x) {
    static const double inv_sqrt_2pi = 0.3989422804014327;
    return inv_sqrt_2pi * std::exp(-0.5 * x * x);
}

}  // namespace quant
