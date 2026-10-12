"""Market tools: the BTC snapshot (spot, DVOL, rates, funding, IV surface) and the IV surface's diagnostics."""

from optra.schemas.tools import IvSurfaceRequest, NoInput
from optra.services import iv_surface as iv_diagnostics
from optra.services.market_data import fetch_btc_snapshot, rate_curve_info
from optra.tools.base import Result, Tool


async def market_snapshot(_: NoInput) -> Result:
    snap = await fetch_btc_snapshot()
    surface = snap.pop("iv_surface")
    rates = rate_curve_info()
    snap["rate_curve_source"], snap["rate_curve_as_of"] = rates["source"], rates["as_of"]
    warnings = ["the US Treasury curve is unavailable: fixed fallback rates are used"] if rates["source"] == "fallback" else []
    return Result(summary=snap, details={"iv_surface": surface}, warnings=warnings,
                  notes=[f"{k}: {v}" for k, v in snap.get("sources", {}).items()])


async def iv_surface(req: IvSurfaceRequest) -> Result:
    summary, details = await iv_diagnostics.diagnose(req.spot, req.strike, req.maturity, req.smile_points,
                                                     req.moneyness_steps, req.tenor_steps,
                                                     req.burst_lower_quantile, req.burst_upper_quantile)
    return Result(summary=summary, details=details)


TOOLS = [
    Tool("market_snapshot", "BTC market snapshot",
         "Live BTC market inputs for pricing: spot (Binance), the DVOL implied-vol index (Deribit), the 3-month US Treasury "
         "rate with the curve from 1 month to 2 years (rate_curve_as_of: its date), the perpetual funding rate as an "
         "annual drift mu, and an implied-vol surface (Deribit marks, the strikes nearest the money per expiry). Each "
         "source falls back to a fixed value offline. Use it to fill spot, vol, rate and mu before pricing a BTC option.",
         NoInput, market_snapshot, "/lab/market/btc", method="GET", engine=False, network=True),
    Tool("iv_surface", "BTC implied-vol surface",
         "The implied-vol surface of the live Deribit BTC chain around a strike and maturity: the ATM term structure, the "
         "smile of the nearest expiry (and every expiry's), the IV interpolated at the strike and maturity (bilinear in "
         "log-moneyness and sqrt(T)), the local skew slope and curvature, a moneyness × maturity grid with its burst and "
         "suppressed zones, and a confidence score. Use it to read the smile or pick a vol for a strike.",
         IvSurfaceRequest, iv_surface, "/lab/market/iv/diagnostics", network=True,
         example={"spot": 82000.0, "strike": 85000.0, "maturity": 0.25}),
]
