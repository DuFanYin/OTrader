"""Multi-leg strategy pricing."""

from optra.schemas.tools import MultiLegRequest
from optra.infra.numerics import multi_leg
from optra.tools.base import Result, Tool


def price_strategy(req: MultiLegRequest) -> Result:
    result = multi_leg(spot=req.spot, rate=req.rate, vol=req.vol, maturity=req.maturity,
                       legs=[leg.engine_payload() for leg in req.legs],
                       dividend_yield=req.dividend_yield, n_paths=req.n_paths)
    legs = result.pop("legs")
    return Result(summary=result, details={"legs": legs})


TOOLS = [
    Tool("price_strategy", "Price a multi-leg strategy",
         "Prices a position of up to ten option legs (calls and puts, long or short, each with its own strike and optionally "
         "its own vol, maturity or forward for Black-76) with Black-Scholes and Monte Carlo, and recognises the strategy "
         "(straddle, strangle, spreads, butterfly, condor, iron condor, calendar or custom). Returns result_summary "
         "(net_bs_price, net_mc_price, net_binomial_price, net_delta, net_vega, strategy_hint) and result_details.legs, one row per leg.",
         MultiLegRequest, price_strategy, "/lab/pricing/multi-leg",
         example={"spot": 100.0, "rate": 0.03, "vol": 0.2, "maturity": 0.5, "legs": [
             {"option_type": "put", "strike": 90.0, "quantity": 1}, {"option_type": "put", "strike": 95.0, "quantity": -1},
             {"option_type": "call", "strike": 105.0, "quantity": -1}, {"option_type": "call", "strike": 110.0, "quantity": 1}]}),
]
