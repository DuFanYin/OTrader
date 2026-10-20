#include "../kernel.h"

#include <cmath>
#include <numbers>

namespace quant {

double clamp_positive(double x, double eps) {
    return x < eps ? eps : x;
}

double clamp_nonnegative(double x) {
    return x < 0.0 ? 0.0 : x;
}

// erfc: full precision in the lower tail, where 1 + erf(x) loses every digit.
double norm_cdf(double x) {
    return 0.5 * std::erfc(-x * 0.70710678118654752440);
}

double norm_pdf(double x) {
    static const double inv_sqrt_2pi = 0.3989422804014327;
    return inv_sqrt_2pi * std::exp(-0.5 * x * x);
}

// Acklam's rational approximation, refined by one Halley step: |error| < 1e-12.
double normal_quantile(double p) {
    constexpr double a[] = {-3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
                            1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00};
    constexpr double b[] = {-5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
                            6.680131188771972e+01, -1.328068155288572e+01};
    constexpr double c[] = {-7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
                            -2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00};
    constexpr double d[] = {7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
                            3.754408661907416e+00};
    constexpr double low = 0.02425;
    double x = 0.0;
    if (p < low) {
        const double q = std::sqrt(-2 * std::log(p));
        x = (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    } else if (p <= 1 - low) {
        const double q = p - 0.5;
        const double r = q * q;
        x = (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) * q /
            (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1);
    } else {
        const double q = std::sqrt(-2 * std::log(1 - p));
        x = -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q + c[5]) /
            ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1);
    }
    const double e = norm_cdf(x) - p;
    const double u = e * std::sqrt(2 * std::numbers::pi) * std::exp(x * x / 2);
    return x - u / (1 + x * u / 2);
}

}  // namespace quant
