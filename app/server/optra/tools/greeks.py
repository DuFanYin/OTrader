"""Greek surfaces."""

from optra.schemas.tools import GreekSurfaceRequest
from optra.infra.numerics import greek_surface as engine_greek_surface
from optra.tools.base import Result, Tool


def greek_surface(req: GreekSurfaceRequest) -> Result:
    result = engine_greek_surface(strike=req.strike, rate=req.rate, vol=req.vol, dividend_yield=req.dividend_yield,
                                  spot_min=req.spot_min, spot_max=req.spot_max, mat_min=req.mat_min, mat_max=req.mat_max,
                                  n_spots=req.n_spots, n_mats=req.n_mats, greek=req.greek)
    summary = {k: result[k] for k in ("greek", "n_spots", "n_mats", "grid_min", "grid_max")}
    details = {k: result[k] for k in ("spots", "maturities", "grid")}
    return Result(summary=summary, details=details)


TOOLS = [
    Tool("greek_surface", "Greek surface",
         "Computes one Black-Scholes Greek of a call over a grid of spots × maturities. Use it to see where an option's "
         "delta, gamma, vega, theta or rho is largest. Returns result_details.spots, maturities and grid (row-major: one "
         "row of maturities per spot) and the range in result_summary.",
         GreekSurfaceRequest, greek_surface, "/lab/greek/surface",
         example={"strike": 100.0, "rate": 0.03, "vol": 0.2, "spot_min": 60.0, "spot_max": 140.0, "greek": "gamma"}),
]
