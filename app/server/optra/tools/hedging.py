"""Delta-hedging comparison."""

from optra.schemas.tools import HedgingRequest
from optra.tools.base import Result, Tool, engine_task


def compare_hedges(req: HedgingRequest) -> Result:
    payload = req.model_dump(exclude={"legs"})
    if req.legs:
        payload["legs"] = [leg.engine_payload() for leg in req.legs]
    return engine_task("hedging", payload)


TOOLS = [
    Tool("compare_hedges", "Compare delta-hedging strategies",
         "Delta-hedges a position on simulated paths of the underlying with four strategies run on the same paths: discrete "
         "delta, delta with transaction costs, threshold rebalancing, and hedging with a mismatched vol. The position is "
         "taken at its Black-Scholes value and held as given in legs (quantity > 0 bought, < 0 sold; each leg may have its "
         "own vol, maturity or forward, and settles in cash at its expiry); without legs it is one option sold at strike "
         "(option_type), a market maker's hedge. P&L = payoffs + hedge − the position's cost, so costs lower it, and ES95 "
         "is the mean loss of the worst 5% of paths. Use it to judge how well a hedge works and what it costs. Returns result_summary (the "
         "P&L mean, std and quantiles of discrete delta hedging, n_legs, horizon, best_strategy) and result_details."
         "strategy_compare (per strategy: mean, std, q05/q50/q95, var95, es95, turnover, transaction_cost), best_strategy "
         "(lowest ES95) and a P&L histogram.",
         HedgingRequest, compare_hedges, "/lab/hedging/run",
         example={"spot": 100.0, "strike": 100.0, "rate": 0.03, "vol": 0.2, "maturity": 0.5, "n_paths": 2000, "legs": [
             {"option_type": "call", "strike": 110.0, "quantity": -1}, {"option_type": "put", "strike": 90.0, "quantity": -1}]}),
]
