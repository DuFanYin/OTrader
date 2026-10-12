"""Explainable QA planner for validation gate outputs."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

QA_RULE_VERSION = "qa-v1"


@dataclass(frozen=True)
class ActionTemplate:
    action_id: str
    param: str
    cost: float
    rationale: str
    covers: tuple[tuple[str, str], ...]


ACTION_TEMPLATES: tuple[ActionTemplate, ...] = (
    ActionTemplate(
        action_id="increase_mc_paths",
        param="n_paths",
        cost=2.0,
        rationale="Higher Monte Carlo paths reduce sampling noise and stabilize tail metrics.",
        covers=(
            ("measure_compare", "var_gap_abs"),
            ("measure_compare", "path_dispersion_gap_abs"),
            ("pricing_batch", "spread_cv"),
            ("pricing_batch", "p95_method_spread"),
        ),
    ),
    ActionTemplate(
        action_id="increase_stats_sample",
        param="stats_n",
        cost=1.0,
        rationale="A larger sample brings the sample moments closer to the exact ones (error ~ 1/sqrt(n)).",
        covers=(("stats", "mean_error"), ("stats", "variance_error")),
    ),
    ActionTemplate(
        action_id="increase_ito_steps",
        param="ito_n",
        cost=1.2,
        rationale="Finer steps shrink the gap in Ito's formula on discrete paths (~ sqrt(dt)).",
        covers=(("ito", "formula_residual"),),
    ),
    ActionTemplate(
        action_id="lower_ito_theta",
        param="ito_theta",
        cost=1.0,
        rationale="With a large theta^2 t the exponential martingale's mean rests on rare paths that sampling misses.",
        covers=(("ito", "expectation_gap"),),
    ),
    ActionTemplate(
        action_id="tighten_simulation_dt",
        param="sim_dt",
        cost=1.5,
        rationale="Smaller dt improves simulation stability for finite terminal values.",
        covers=(("simulation", "terminal_finite"),),
    ),
    ActionTemplate(
        action_id="increase_rebalances",
        param="n_rebalances",
        cost=1.8,
        rationale="More frequent hedge rebalancing narrows hedging PnL dispersion.",
        covers=(("hedging", "normalized_std"),),
    ),
    ActionTemplate(
        action_id="increase_convergence_ladder",
        param="conv_steps",
        cost=1.4,
        rationale="Denser and higher convergence ladder reduces residual pricing error.",
        covers=(
            ("convergence", "best_abs_error"),
            ("convergence", "monotonicity_break_count"),
            ("convergence", "last_two_improvement_ratio"),
        ),
    ),
    ActionTemplate(
        action_id="increase_pde_grid",
        param="pde_s_steps",
        cost=2.2,
        rationale="Finer PDE grid improves numerical consistency against analytic baseline.",
        covers=(("pde", "price_vs_bs_gap_abs"),),
    ),
)


def _safe_num(v: Any, default: float = 0.0) -> float:
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def _severity(row: dict[str, Any]) -> str:
    threshold = abs(_safe_num(row.get("threshold"), 0.0))
    violation = abs(_safe_num(row.get("violation"), _safe_num(row.get("excess"), 0.0)))
    denom = threshold if threshold > 1e-10 else 1.0
    ratio = violation / denom
    if ratio >= 1.0:
        return "high"
    if ratio >= 0.4:
        return "medium"
    return "low"


def _propose_value(param: str, req: dict[str, Any]) -> Any:
    if param == "n_paths":
        return max(int(_safe_num(req.get("n_paths"), 20000)), 20000) * 4
    if param == "stats_n":
        return max(int(_safe_num(req.get("stats_n"), 10000)), 10000) * 4
    if param == "ito_theta":
        return round(_safe_num(req.get("ito_theta"), 0.7) / 2, 4)
    if param == "ito_n":
        return max(int(_safe_num(req.get("ito_n"), 2000)), 2000) * 2
    if param == "sim_dt":
        return max(_safe_num(req.get("sim_dt"), 0.01) * 0.5, 0.0005)
    if param == "n_rebalances":
        return max(int(_safe_num(req.get("n_rebalances"), 52)), 52) * 2
    if param == "conv_steps":
        base = req.get("conv_steps") or [10, 20, 40, 80, 120, 200, 320, 500]
        ladder = [int(v) for v in base if _safe_num(v) > 1]
        if not ladder:
            ladder = [10, 20, 40, 80, 120, 200, 320, 500]
        ext = max(ladder) * 2
        if ext not in ladder:
            ladder.append(ext)
        return sorted(set(ladder))
    if param == "pde_s_steps":
        s = int(_safe_num(req.get("pde_s_steps"), 200))
        t = int(_safe_num(req.get("pde_t_steps"), 200))
        return {"pde_s_steps": max(s * 2, 240), "pde_t_steps": max(t * 2, 240)}
    return req.get(param)


def _effect_range(violations: list[float]) -> dict[str, float]:
    if not violations:
        return {"expected_fail_drop": 0.0, "expected_max_excess_drop": 0.0}
    mean_v = sum(violations) / len(violations)
    max_v = max(violations)
    return {
        "expected_fail_drop": min(1.0, 0.35 + min(0.45, mean_v)),
        "expected_max_excess_drop": min(1.0, 0.30 + min(0.50, max_v)),
    }


def build_explainable_qa(rows: list[dict[str, Any]], summary: dict[str, Any], req: dict[str, Any]) -> dict[str, Any]:
    failed_rows = [r for r in rows if r.get("status") == "fail"]
    if not failed_rows:
        return {
            "qa_rule_version": QA_RULE_VERSION,
            "decision_reasoning": "All validation checks passed; no remediation actions needed.",
            "root_causes": [],
            "actions_ranked": [],
            "minimal_fix_set": [],
            "what_if_preview": {
                "projected_gate_decision": "go",
                "projected_checks_failed": 0,
                "projected_fail_rate": 0.0,
            },
        }

    sorted_failed = sorted(
        failed_rows,
        key=lambda r: _safe_num(r.get("violation"), _safe_num(r.get("excess"), 0.0)),
        reverse=True,
    )
    root_causes = [
        {
            "capability": r.get("capability"),
            "metric": r.get("metric"),
            "severity": _severity(r),
            "violation": _safe_num(r.get("violation"), _safe_num(r.get("excess"), 0.0)),
            "interpretation": r.get("interpretation"),
        }
        for r in sorted_failed[:6]
    ]

    failed_keys = {(str(r.get("capability")), str(r.get("metric"))) for r in failed_rows}
    action_candidates: list[dict[str, Any]] = []
    for template in ACTION_TEMPLATES:
        covered = [r for r in failed_rows if (r.get("capability"), r.get("metric")) in set(template.covers)]
        if not covered:
            continue
        violations = [_safe_num(r.get("violation"), _safe_num(r.get("excess"), 0.0)) for r in covered]
        effects = _effect_range(violations)
        covered_pairs = [(str(r.get("capability")), str(r.get("metric"))) for r in covered]
        expected_gain = effects["expected_fail_drop"] + effects["expected_max_excess_drop"]
        action_candidates.append(
            {
                "action_id": template.action_id,
                "priority_score": expected_gain / template.cost,
                "cost_score": template.cost,
                "rationale": template.rationale,
                "param": template.param,
                "current_value": req.get(template.param),
                "proposed_value": _propose_value(template.param, req),
                "covers_metrics": [f"{cap}.{metric}" for cap, metric in covered_pairs],
                "effects": effects,
                "covered_pairs": covered_pairs,
            }
        )

    action_candidates.sort(key=lambda a: a["priority_score"], reverse=True)

    remaining = set(failed_keys)
    minimal_fix_set: list[dict[str, Any]] = []
    for action in action_candidates:
        coverage = set(action["covered_pairs"])
        if not coverage & remaining:
            continue
        minimal_fix_set.append(
            {
                "action_id": action["action_id"],
                "proposed_value": action["proposed_value"],
                "covers_metrics": action["covers_metrics"],
                "priority_score": action["priority_score"],
            }
        )
        remaining -= coverage
        if not remaining:
            break

    covered_count = len(failed_keys) - len(remaining)
    projected_failed = max(0, len(failed_keys) - covered_count)
    projected_fail_rate = projected_failed / max(1, len(rows))
    block_mode = summary.get("validation_mode") == "blocking"
    projected_decision = "go" if projected_failed == 0 else ("blocked" if block_mode else "warning")

    for action in action_candidates:
        action.pop("covered_pairs", None)

    return {
        "qa_rule_version": QA_RULE_VERSION,
        "decision_reasoning": (
            f"{len(failed_rows)} checks failed across {len({r.get('capability') for r in failed_rows})} capabilities; "
            "actions are ranked by expected risk-reduction per implementation cost."
        ),
        "root_causes": root_causes,
        "actions_ranked": action_candidates,
        "minimal_fix_set": minimal_fix_set,
        "what_if_preview": {
            "projected_gate_decision": projected_decision,
            "projected_checks_failed": projected_failed,
            "projected_fail_rate": projected_fail_rate,
            "covered_failed_checks": covered_count,
            "total_failed_checks": len(failed_keys),
        },
    }

