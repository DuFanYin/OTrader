// Calibration tasks: implied vols (one, or a batch on one spot), Heston fitted to call prices, and
// a Heston call price.
#include "json_io.h"
#include "quant/kernel.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace quant::analytics {

std::string run_implied_vol_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src, {"market_price", "spot", "strike", "rate", "maturity"},
                                   "missing_required_implied_vol_fields");
    if (!req) return req.error();
    const json& j = *req;
    const double price = jv(j, "market_price", 0.0), spot = jv(j, "spot", 0.0), strike = jv(j, "strike", 0.0);
    const double rate = jv(j, "rate", 0.0), maturity = jv(j, "maturity", 1.0), q = jv(j, "dividend_yield", 0.0);
    const double iv = bs_implied_vol(price, spot, strike, rate, maturity, q);
    const bool converged = iv > 0.0;
    const double error =
        converged ? std::abs(black_price(true, spot_forward(spot, rate, q, maturity), strike, maturity, rate, iv) - price)
                  : 0.0;
    return ok_response({{"implied_vol", iv}, {"converged", converged}, {"final_error", error}}, json::object(), t0_ms);
}

std::string run_implied_vol_batch_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src, {"spot", "rate"}, "missing_required_implied_vol_batch_fields");
    if (!req) return req.error();
    const json& j = *req;
    for (const std::string arr : {"market_prices", "strikes", "expiries"}) {
        if (!j.contains(arr) || !j[arr].is_array()) return err_response("missing_array_" + arr);
    }
    const auto prices = j["market_prices"].get<std::vector<double>>();
    const auto strikes = j["strikes"].get<std::vector<double>>();
    const auto expiries = j["expiries"].get<std::vector<double>>();
    if (prices.size() != strikes.size() || strikes.size() != expiries.size())
        return err_response("array_length_mismatch");
    const std::size_t n = prices.size();
    std::vector<double> ivs(n);
    std::vector<uint8_t> converged(n);  // not vector<bool>: the kernel writes a span of it
    implied_vol_batch(prices, strikes, expiries, jv(j, "spot", 0.0), jv(j, "rate", 0.0), jv(j, "dividend_yield", 0.0),
                      ivs, converged);
    const int n_converged = static_cast<int>(std::ranges::count(converged, 1));
    json flags = json::array();
    for (uint8_t c : converged) flags.push_back(c != 0);
    json summary = {{"n_points", static_cast<int>(n)},
                    {"n_converged", n_converged},
                    {"convergence_rate", n == 0 ? 0.0 : static_cast<double>(n_converged) / n}};
    return ok_response(std::move(summary), {{"ivs", ivs}, {"converged", std::move(flags)}}, t0_ms);
}

std::string run_heston_price_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src, {"spot", "strike", "rate", "maturity"}, "missing_required_heston_price_fields");
    if (!req) return req.error();
    const json& j = *req;
    const double price = heston_call_price(jv(j, "spot", 0.0), jv(j, "strike", 0.0), jv(j, "rate", 0.0),
                                           jv(j, "maturity", 1.0), jv(j, "v0", 0.04), jv(j, "kappa", 1.5),
                                           jv(j, "theta", 0.04), jv(j, "xi", 0.5), jv(j, "rho", -0.7));
    return ok_response({{"heston_price", price}}, json::object(), t0_ms);
}

std::string run_heston_calibrate_json(const std::string& src, double t0_ms) {
    const auto req = parse_request(src, {"spot", "rate"}, "missing_required_heston_calibrate_fields");
    if (!req) return req.error();
    const json& j = *req;
    for (const std::string arr : {"market_strikes", "market_maturities", "market_prices"}) {
        if (!j.contains(arr) || !j[arr].is_array()) return err_response("missing_array_" + arr);
    }
    const auto strikes = j["market_strikes"].get<std::vector<double>>();
    const auto maturities = j["market_maturities"].get<std::vector<double>>();
    const auto prices = j["market_prices"].get<std::vector<double>>();
    if (prices.empty()) return err_response("calibration_data_empty");
    const double spot = jv(j, "spot", 0.0), rate = jv(j, "rate", 0.0);
    // The fit is quant's, the one the trading engine's strategies use.
    const HestonFit fit = heston_calibrate(spot, rate, strikes, maturities, prices,
                                           {jv(j, "init_v0", 0.04), jv(j, "init_kappa", 1.5), jv(j, "init_theta", 0.04),
                                            jv(j, "init_xi", 0.5), jv(j, "init_rho", -0.7)},
                                           jv(j, "max_iter", 500));
    const HestonParams& h = fit.params;
    const std::size_t n = prices.size();
    std::vector<double> model(n), residuals(n);
    double max_err = 0.0, squares = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        model[i] = heston_call_price(spot, strikes[i], rate, maturities[i], h.v0, h.kappa, h.theta, h.xi, h.rho);
        residuals[i] = model[i] - prices[i];
        max_err = std::max(max_err, std::abs(residuals[i]));
        squares += residuals[i] * residuals[i];
    }
    json summary = {{"v0", h.v0},       {"kappa", h.kappa},
                    {"theta", h.theta}, {"xi", h.xi},
                    {"rho", h.rho},     {"rmse", std::sqrt(squares / n)},
                    {"max_abs_error", max_err}, {"iterations", fit.iterations},
                    {"converged", fit.converged}};
    return ok_response(std::move(summary), {{"model_prices", model}, {"residuals", residuals}}, t0_ms);
}

}  // namespace quant::analytics
