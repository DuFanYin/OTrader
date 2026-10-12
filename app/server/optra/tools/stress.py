"""The stress library: scenario packs, and an option or a position repriced under them."""

from optra.schemas.tools import NoInput, StressLibraryRequest
from optra.services.stress_library import _PACK_SPECS, run_stress_library
from optra.tools.base import Result, Tool, ToolError

_PACK_META = {
    "core4":       {"name": "Core 4",       "description": "Four canonical market shocks covering vol, rate, spot gap, and correlation breakdown."},
    "vol_first":   {"name": "Vol First",    "description": "Three volatility-driven scenarios: regime shift, vol crush, and skew panic."},
    "rates_first": {"name": "Rates First",  "description": "Three rate-driven scenarios: front-end jump, policy easing, and rate/vol divergence."},
    "crash_kit":   {"name": "Crash Kit",    "description": "Three severe crash scenarios: crash day, aftershock, and liquidity vacuum."},
}


def stress_packs(_: NoInput) -> Result:
    packs = []
    for pack_id, scenarios in _PACK_SPECS.items():
        meta = _PACK_META.get(pack_id, {"name": pack_id, "description": ""})
        packs.append({
            "id": pack_id,
            "name": meta["name"],
            "description": meta["description"],
            "scenario_count": len(scenarios),
            "scenarios": [{"name": s.get("name", ""), "description": s.get("description", ""),
                           "spot_mult_delta": s.get("spot_mult_delta", 0.0), "vol_mult_delta": s.get("vol_mult_delta", 0.0),
                           "rate_shift": s.get("rate_shift", 0.0)} for s in scenarios],
            "version": "1.0",
        })
    return Result(summary={"packs": packs})


def stress_test(req: StressLibraryRequest) -> Result:
    try:
        result = run_stress_library(
            spot=req.spot, strike=req.strike, rate=req.rate, vol=req.vol, maturity=req.maturity,
            n_paths=req.n_paths, dividend_yield=req.dividend_yield, n_rebalances=req.n_rebalances,
            hedge_paths=req.hedge_paths, transaction_cost_bps=req.transaction_cost_bps,
            rebalance_threshold=req.rebalance_threshold, vol_mismatch_mult=req.vol_mismatch_mult,
            stress_pack=req.stress_pack, stress_severity=req.stress_severity,
            include_hedge_compare=req.include_hedge_compare,
            legs=[leg.engine_payload() for leg in req.legs] if req.legs else None,
            option_type=req.option_type,
        )
    except ValueError as exc:
        raise ToolError(str(exc)) from exc
    return Result(summary=result["summary"], details=result["details"], notes=["stress_library"])


TOOLS = [
    Tool("stress_packs", "List the stress scenario packs",
         "Lists the stress scenario packs stress_test can run, each with its scenarios (spot, vol and rate shocks). "
         "Returns result_summary.packs.",
         NoInput, stress_packs, "/lab/stress/packs", method="GET", engine=False),
    Tool("stress_test", "Stress an option or a position",
         "Reprices one option, or a position of legs, under every scenario of a stress pack (core4, vol_first, rates_first, "
         "crash_kit) at a severity, with Black-Scholes, Monte Carlo and binomial, splits each P&L into spot, vol and rate, "
         "and reruns the hedge comparison in each scenario (compare_hedges, on the shocked option or position). Use it to find what hurts a position most. "
         "Returns result_summary (worst_scenario, worst_mc_shift, robustness, key driver, the cross-scenario summary) and "
         "result_details (per-scenario rows, attribution, rankings).",
         StressLibraryRequest, stress_test, "/lab/stress/run", cost="seconds",
         example={"spot": 100.0, "strike": 100.0, "rate": 0.03, "vol": 0.2, "maturity": 0.5, "stress_pack": "crash_kit",
                  "include_hedge_compare": False}),
]
