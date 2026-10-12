"""Standardized stress scenario library and execution helpers."""

from __future__ import annotations

from dataclasses import dataclass
import json
from pathlib import Path
from typing import Any

from optra.infra.numerics import run_engine_task


@dataclass(frozen=True)
class StressScenario:
    name: str
    description: str
    spot_mult: float = 1.0
    vol_mult: float = 1.0
    rate_shift: float = 0.0


_SEVERITY_SCALE = {
    "mild": 0.7,
    "moderate": 1.0,
    "severe": 1.5,
}


def _load_pack_specs() -> dict[str, list[dict[str, Any]]]:
    path = Path(__file__).with_name("stress_packs.json")
    if not path.exists():
        raise RuntimeError(f"stress pack config missing: {path}")
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:
        raise RuntimeError(f"failed to parse stress pack config: {path}") from exc
    if not isinstance(payload, dict):
        raise RuntimeError("stress pack config must be an object")
    normalized: dict[str, list[dict[str, Any]]] = {}
    for k, v in payload.items():
        if isinstance(k, str) and isinstance(v, list):
            normalized[k] = [x for x in v if isinstance(x, dict)]
    if not normalized:
        raise RuntimeError("stress pack config has no valid packs")
    if "core4" not in normalized:
        raise RuntimeError("stress pack config must include 'core4'")
    return normalized


_PACK_SPECS = _load_pack_specs()


def _scenario_from_spec(spec: dict[str, Any], scale: float) -> StressScenario:
    spot_floor = float(spec.get("spot_mult_floor", 0.05))
    vol_floor = float(spec.get("vol_mult_floor", 0.05))
    spot_mult = max(spot_floor, 1.0 + float(spec.get("spot_mult_delta", 0.0)) * scale)
    vol_mult = max(vol_floor, 1.0 + float(spec.get("vol_mult_delta", 0.0)) * scale)
    rate_shift = float(spec.get("rate_shift", 0.0)) * scale
    return StressScenario(
        name=str(spec.get("name", "Unnamed Scenario")),
        description=str(spec.get("description", "")),
        spot_mult=spot_mult,
        vol_mult=vol_mult,
        rate_shift=rate_shift,
    )


def _clamp_rate(rate: float) -> float:
    return max(-1.0, min(1.0, rate))


def _run_pricing(payload: dict[str, Any]) -> dict[str, Any]:
    r = run_engine_task("pricing", payload)
    return r.get("result_summary", {}) if r.get("status") != "error" else {}


def _run_multi_leg(payload: dict[str, Any]) -> dict[str, Any]:
    r = run_engine_task("multi_leg", payload)
    if r.get("status") == "error":
        raise ValueError(f"multi-leg pricing failed: {r.get('error', {}).get('message', 'unknown')}")
    return r.get("result_summary", {})


def _run_hedging(payload: dict[str, Any]) -> dict[str, Any]:
    r = run_engine_task("hedging", payload)
    if r.get("status") == "error":
        return {"summary": {}, "details": {}}
    return {
        "summary": r.get("result_summary", {}),
        "details": r.get("result_details", {}),
    }


def _pick_pack(stress_pack: str, scale: float) -> list[StressScenario]:
    raw_specs = _PACK_SPECS.get(stress_pack)
    if raw_specs is None:
        raise ValueError(f"unknown stress_pack: {stress_pack}")
    return [_scenario_from_spec(spec, scale) for spec in raw_specs]


def scenarios(stress_pack: str = "core4", stress_severity: str = "moderate") -> list[StressScenario]:
    """A pack's scenarios at a severity (mild, moderate, severe)."""
    if stress_severity not in _SEVERITY_SCALE:
        raise ValueError(f"unknown severity: {stress_severity}")
    return _pick_pack(stress_pack, _SEVERITY_SCALE[stress_severity])


def packs() -> list[str]:
    return list(_PACK_SPECS)


def run_stress_library(
    *,
    spot: float,
    strike: float,
    rate: float,
    vol: float,
    maturity: float,
    n_paths: int,
    dividend_yield: float,
    n_rebalances: int,
    hedge_paths: int,
    transaction_cost_bps: float,
    rebalance_threshold: float,
    vol_mismatch_mult: float,
    stress_pack: str,
    stress_severity: str,
    include_hedge_compare: bool,
    legs: list[dict[str, Any]] | None = None,
    option_type: str = "call",
) -> dict[str, Any]:
    """
    Reprice under each scenario of a stress pack.

    Single-option mode (legs is None): the option at `strike`, with MC / BS /
    binomial, plus an optional delta-hedge comparison per scenario.

    Portfolio mode (legs given, multi-leg contract): the whole position is
    repriced via the multi-leg engine (Black-Scholes, Monte Carlo and a binomial
    tree per leg). The spot shock also scales each leg's `forward`, the vol shock
    scales each leg's own `vol`; the hedge comparison hedges the shocked position.
    """
    scale = _SEVERITY_SCALE.get(stress_severity, 1.0)
    scenarios = _pick_pack(stress_pack, scale)
    portfolio = bool(legs)

    def shocked(spot_mult: float, vol_mult: float) -> list[dict[str, Any]]:
        out = []
        for leg in legs or []:
            leg = dict(leg)
            if leg.get("forward"):
                leg["forward"] = leg["forward"] * spot_mult
            if leg.get("vol"):
                leg["vol"] = max(1e-8, leg["vol"] * vol_mult)
            out.append(leg)
        return out

    def price(s: float, v: float, r: float, spot_mult: float = 1.0, vol_mult: float = 1.0) -> dict[str, Any]:
        if not portfolio:
            p = _run_pricing({"spot": s, "strike": strike, "rate": r, "vol": v, "maturity": maturity,
                              "n_paths": n_paths, "dividend_yield": dividend_yield, "option_type": option_type})
            return {"mc": float(p.get("mc", 0.0)), "bs": float(p.get("bs", 0.0)),
                    "binomial": float(p.get("binomial", 0.0))}
        p = _run_multi_leg({"spot": s, "rate": r, "vol": v, "maturity": maturity,
                            "dividend_yield": dividend_yield, "n_paths": n_paths, "legs": shocked(spot_mult, vol_mult)})
        return {"mc": float(p.get("net_mc_price", 0.0)), "bs": float(p.get("net_bs_price", 0.0)),
                "binomial": float(p.get("net_binomial_price", 0.0))}

    base = price(spot, vol, rate)
    base_mc = base["mc"]
    base_bs = base["bs"]
    base_bin = base["binomial"]

    rows: list[dict[str, Any]] = []
    for sc in scenarios:
        shocked_spot = max(1e-8, spot * sc.spot_mult)
        shocked_vol = max(1e-8, vol * sc.vol_mult)
        shocked_rate = _clamp_rate(rate + sc.rate_shift)
        p = price(shocked_spot, shocked_vol, shocked_rate, sc.spot_mult, sc.vol_mult)
        mc, bs, bn = p["mc"], p["bs"], p["binomial"]

        hedge_metrics = {}
        if include_hedge_compare:
            h = _run_hedging(
                {
                    "spot": shocked_spot,
                    "strike": strike,
                    "rate": shocked_rate,
                    "vol": shocked_vol,
                    "maturity": maturity,
                    "n_rebalances": n_rebalances,
                    "n_paths": hedge_paths,
                    "transaction_cost_bps": transaction_cost_bps,
                    "rebalance_threshold": rebalance_threshold,
                    "vol_mismatch_mult": vol_mismatch_mult,
                    "dividend_yield": dividend_yield,
                    "option_type": option_type,
                    **({"legs": shocked(sc.spot_mult, sc.vol_mult)} if portfolio else {}),
                }
            )
            best = h["details"].get("best_strategy", {}) if isinstance(h["details"], dict) else {}
            best_stats = (h["details"].get("strategy_compare") or {}).get(best.get("name"), {})
            hedge_metrics = {
                "normalized_std": h["summary"].get("normalized_std"),
                "var95": best_stats.get("var95"),
                "es95": best.get("es95"),
                "best_strategy": best.get("name"),
                "best_reason": best.get("reason"),
            }

        rows.append(
            {
                "scenario": sc.name,
                "description": sc.description,
                "spot": shocked_spot,
                "vol": shocked_vol,
                "rate": shocked_rate,
                "mc": mc,
                "bs": bs,
                "binomial": bn,
                "mc_shift_vs_base": mc - base_mc,
                "bs_shift_vs_base": bs - base_bs,
                "binomial_shift_vs_base": (bn - base_bin) if bn is not None and base_bin is not None else None,
                "severity_score": abs(mc - base_mc),
                "portfolio_correlation_score": (
                    abs((shocked_spot - spot) / max(spot, 1e-12)) * 0.40
                    + abs((shocked_vol - vol) / max(vol, 1e-12)) * 0.35
                    + abs(shocked_rate - rate) * 12.0 * 0.25
                ),
                "hedge": hedge_metrics,
                "attribution": {
                    "spot_component": shocked_spot - spot,
                    "vol_component": shocked_vol - vol,
                    "rate_component": shocked_rate - rate,
                },
            }
        )

    ranked = sorted(rows, key=lambda r: r["severity_score"], reverse=True)
    correlation_ranked = sorted(rows, key=lambda r: r["portfolio_correlation_score"], reverse=True)
    resilience_ranked = sorted(
        rows,
        key=lambda r: (
            r.get("hedge", {}).get("normalized_std") if r.get("hedge", {}).get("normalized_std") is not None else 1e9,
            abs(r.get("mc_shift_vs_base", 0.0)),
        ),
    )
    worst = ranked[0] if ranked else None

    # Cross-scenario aggregation
    cross_summary = _cross_scenario_summary(rows, spot)

    return {
        "summary": {
            "mode": "portfolio" if portfolio else "single_option",
            "n_legs": len(legs) if portfolio else 1,
            "hedge_compare": include_hedge_compare,
            "pack": stress_pack,
            "severity": stress_severity,
            "scenario_count": len(rows),
            "base_mc": base_mc,
            "worst_scenario": worst["scenario"] if worst else None,
            "worst_mc_shift": worst["mc_shift_vs_base"] if worst else 0.0,
        },
        "details": {
            "rows": rows,
            "severity_ranking": [
                {"scenario": r["scenario"], "severity_score": r["severity_score"], "mc_shift_vs_base": r["mc_shift_vs_base"]}
                for r in ranked
            ],
            "portfolio_correlation_ranking": [
                {
                    "scenario": r["scenario"],
                    "portfolio_correlation_score": r["portfolio_correlation_score"],
                    "mc_shift_vs_base": r["mc_shift_vs_base"],
                }
                for r in correlation_ranked
            ],
            "hedge_resilience_ranking": [
                {
                    "scenario": r["scenario"],
                    "hedge_normalized_std": r.get("hedge", {}).get("normalized_std"),
                    "hedge_best_strategy": r.get("hedge", {}).get("best_strategy"),
                }
                for r in resilience_ranked
            ],
            "tornado_points": [{"name": r["scenario"], "value": r["mc_shift_vs_base"]} for r in ranked],
            "cross_scenario_summary": cross_summary,
        },
    }


def _cross_scenario_summary(rows: list[dict], spot: float) -> dict:
    if not rows:
        return {}

    impacts = [abs(r["mc_shift_vs_base"]) for r in rows]
    mean_impact = sum(impacts) / len(impacts)
    robustness_score = max(0.0, min(1.0, 1.0 - mean_impact / max(spot, 1e-8)))

    worst_row = max(rows, key=lambda r: abs(r["mc_shift_vs_base"]))

    # Key driver: whichever attribution component has highest mean absolute value
    spot_contribs = [abs(r["attribution"]["spot_component"]) for r in rows]
    vol_contribs  = [abs(r["attribution"]["vol_component"])  for r in rows]
    rate_contribs = [abs(r["attribution"]["rate_component"]) for r in rows]
    mean_spot = sum(spot_contribs) / len(spot_contribs)
    mean_vol  = sum(vol_contribs)  / len(vol_contribs)
    mean_rate = sum(rate_contribs) / len(rate_contribs)
    total = max(mean_spot + mean_vol + mean_rate, 1e-12)
    driver_vals = {"spot": mean_spot, "vol": mean_vol, "rate": mean_rate}
    key_driver = max(driver_vals, key=driver_vals.get)  # type: ignore[arg-type]
    key_driver_pct = round(driver_vals[key_driver] / total * 100.0, 2)

    threshold = 0.05 * spot
    breaching = sum(1 for imp in impacts if imp > threshold)

    # Hedge resilience from hedge.es95
    es95_vals = [
        r.get("hedge", {}).get("es95")
        for r in rows
        if r.get("hedge", {}).get("es95") is not None
    ]
    hedge_resilience_mean  = sum(es95_vals) / len(es95_vals) if es95_vals else None
    hedge_resilience_worst = max(es95_vals) if es95_vals else None

    scenario_ranking = [
        {
            "rank": i + 1,
            "name": r["scenario"],
            "pnl_impact": round(r["mc_shift_vs_base"], 6),
            "severity_score": round(r["severity_score"], 6),
        }
        for i, r in enumerate(sorted(rows, key=lambda r: r["severity_score"], reverse=True))
    ]

    return {
        "robustness_score": round(robustness_score, 4),
        "worst_scenario": worst_row["scenario"],
        "worst_pnl_impact": round(worst_row["mc_shift_vs_base"], 6),
        "key_driver": key_driver,
        "key_driver_contribution_pct": key_driver_pct,
        "scenarios_breaching_threshold": breaching,
        "hedge_resilience_mean": round(hedge_resilience_mean, 6) if hedge_resilience_mean is not None else None,
        "hedge_resilience_worst": round(hedge_resilience_worst, 6) if hedge_resilience_worst is not None else None,
        "scenario_ranking": scenario_ranking,
    }

