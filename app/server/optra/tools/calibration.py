"""Calibration: implied vol (Brent) and Heston (Nelder-Mead)."""

from optra.schemas.tools import (
    HestonCalibrateRequest,
    HestonPriceRequest,
    ImpliedVolBatchRequest,
    ImpliedVolRequest,
)
from optra.infra.numerics import heston_calibrate, heston_price as engine_heston_price, implied_vol_batch as engine_iv_batch
from optra.infra.numerics import implied_vol_single
from optra.tools.base import Result, Tool

# The calibration's bounds per parameter (engine/quant/analytics/calibration_engine.cpp): a fitted value this close to one is
# not pinned down by the quotes.
HESTON_BOUNDS = {"v0": (1e-4, 2.0), "kappa": (1e-3, 20.0), "theta": (1e-4, 2.0), "xi": (1e-4, 3.0), "rho": (-0.999, 0.999)}


def heston_bound_warnings(params: dict) -> list[str]:
    """A warning for each fitted Heston parameter that ended at a bound of the calibration."""
    at = [f"{k} = {params[k]:.4g}" for k, (lo, hi) in HESTON_BOUNDS.items()
          if min(abs(params[k] - lo), abs(params[k] - hi)) <= 0.01 * (hi - lo)]
    return [f"Heston parameters at a bound of the calibration ({', '.join(at)}): the quotes do not pin them down, so read "
            "them with care even when the fit is good"] if at else []


def implied_vol(req: ImpliedVolRequest) -> Result:
    return Result(summary=implied_vol_single(market_price=req.market_price, spot=req.spot, strike=req.strike, rate=req.rate,
                                             maturity=req.maturity, dividend_yield=req.dividend_yield))


def implied_vol_batch(req: ImpliedVolBatchRequest) -> Result:
    result = engine_iv_batch(market_prices=req.market_prices, strikes=req.strikes, expiries=req.expiries,
                             spot=req.spot, rate=req.rate, dividend_yield=req.dividend_yield)
    return Result(
        summary={"n_converged": result.get("n_converged", 0), "convergence_rate": result.get("convergence_rate", 0.0),
                 "n_points": len(req.market_prices)},
        details={"ivs": result.get("ivs", []), "converged": result.get("converged", [])},
    )


def heston_price(req: HestonPriceRequest) -> Result:
    price = engine_heston_price(spot=req.spot, strike=req.strike, rate=req.rate, maturity=req.maturity,
                                v0=req.v0, kappa=req.kappa, theta=req.theta, xi=req.xi, rho=req.rho)
    return Result(summary={"heston_price": price})


def calibrate_heston(req: HestonCalibrateRequest) -> Result:
    result = heston_calibrate(
        spot=req.spot, rate=req.rate, market_strikes=req.market_strikes, market_maturities=req.market_maturities,
        market_prices=req.market_prices, dividend_yield=req.dividend_yield,
        init_v0=req.init_v0, init_kappa=req.init_kappa, init_theta=req.init_theta, init_xi=req.init_xi,
        init_rho=req.init_rho, max_iter=req.max_iter,
    )
    summary = {k: result[k] for k in ("v0", "kappa", "theta", "xi", "rho", "rmse", "max_abs_error", "iterations", "converged")}
    # The fit's quality from the RMSE as a share of the average quote, so it reads the same at any price level
    scale = sum(abs(p) for p in req.market_prices) / max(len(req.market_prices), 1)
    rel = result.get("rmse", float("inf")) / scale if scale > 0 else float("inf")
    summary["rmse_rel"] = rel
    summary["fit_quality"] = "good" if rel < 0.02 else "fair" if rel < 0.05 else "poor"
    warnings = [] if summary["fit_quality"] != "poor" else [f"poor fit: RMSE is {rel:.1%} of the average quote"]
    warnings += heston_bound_warnings(summary)
    return Result(summary=summary, details={k: result[k] for k in ("model_prices", "residuals")}, warnings=warnings)


CALL = {"spot": 100.0, "strike": 100.0, "rate": 0.03, "maturity": 0.5}

TOOLS = [
    Tool("implied_vol", "Implied volatility",
         "Finds the Black-Scholes volatility that reproduces an observed European call price (Brent's method). Returns "
         "result_summary.implied_vol, converged, and final_error (the price error left).",
         ImpliedVolRequest, implied_vol, "/lab/calibration/iv", example={**CALL, "market_price": 6.5}),
    Tool("implied_vol_batch", "Implied volatilities",
         "Implied vols of many observed call prices at once (strikes and expiries in the same order as the prices), e.g. to "
         "build a smile. Returns result_details.ivs and converged, one per price.",
         ImpliedVolBatchRequest, implied_vol_batch, "/lab/calibration/iv/batch",
         example={"spot": 100.0, "rate": 0.03, "market_prices": [9.9, 6.5, 4.0], "strikes": [95.0, 100.0, 105.0],
                  "expiries": [0.5, 0.5, 0.5]}),
    Tool("heston_price", "Heston price",
         "Prices a European call in the Heston stochastic-volatility model (characteristic function, Gil-Pelaez inversion). "
         "Returns result_summary.heston_price.",
         HestonPriceRequest, heston_price, "/lab/calibration/heston/price", example=CALL),
    Tool("calibrate_heston", "Calibrate Heston",
         "Fits the five Heston parameters to observed call prices (bounded Nelder-Mead on the RMSE). Use it to describe a "
         "smile with a model. Returns result_summary (v0, kappa, theta, xi, rho, rmse, rmse_rel as a share of the average "
         "quote, fit_quality good / fair / poor) and the model prices and residuals per quote.",
         HestonCalibrateRequest, calibrate_heston, "/lab/calibration/heston", cost="seconds",
         example={"spot": 100.0, "rate": 0.03, "market_strikes": [90.0, 100.0, 110.0, 90.0, 100.0, 110.0],
                  "market_maturities": [0.25, 0.25, 0.25, 1.0, 1.0, 1.0],
                  "market_prices": [11.2, 4.4, 1.2, 15.0, 9.4, 5.2]}),
]
