"""An optimiser over a strategy's settings: Bayesian optimisation, its steps computed by the numerics library
(engine/quant/optimizer/bayesian.cpp, the task "optimize"). Each round's settings are an experiment of the family,
registered and counted before they run like any (ledger.submit, the check "optimizer_round"); once the rounds are
done, every setting tried is one experiment more (all its trials reused, nothing runs again): the experiment the
analysis, the robustness checks and the promotion gates read, so the best is judged against every try."""

from __future__ import annotations

import math
from typing import Any, Literal

from optra.infra import numerics
from optra.research import jobs, ledger
from optra.research.space import within
from optra.research.strategies import strategy_classes

ROUND = "optimizer_round"  # an experiment's check: one step of an optimiser, not where a version is found
OBJECTIVES = ("sharpe", "net", "expectancy_r")


def _ranges(strategy: str, space: dict[str, Any]) -> tuple[list[str], list[float], list[float], list[bool]]:
    """The space's parameters, each {"range": [lo, hi]} within the class's bounds."""
    params = {p["name"]: p for p in strategy_classes().get(strategy) or []}
    if not params:
        raise ValueError(f"unknown strategy {strategy}")
    names, lower, upper, whole = [], [], [], []
    for name, spec in sorted(space.items()):
        if name not in params:
            raise ValueError(f"{strategy} has no parameter {name}")
        if not isinstance(spec, dict) or "range" not in spec:
            raise ValueError(f"{name}: the optimiser searches ranges, {{'range': [lo, hi]}}")
        lo, hi = (float(v) for v in spec["range"])
        p = params[name]
        if not p["min"] <= lo <= hi <= p["max"]:
            raise ValueError(f"{name}: [{lo:g}, {hi:g}] is not within [{p['min']:g}, {p['max']:g}]")
        names.append(name)
        lower.append(lo)
        upper.append(hi)
        whole.append(bool(p["integer"]))
    if not names:
        raise ValueError("the optimiser needs at least one range in the space")
    return names, lower, upper, whole


def _value(summary: dict[str, Any] | None, objective: str) -> float | None:
    v = (summary or {}).get(objective)
    return float(v) if isinstance(v, (int, float)) and math.isfinite(v) else None


def optimize(*, family: str, strategy: str, days: list[str], space: dict[str, Any], costs: ledger.Costs,
             objective: Literal["sharpe", "net", "expectancy_r"] = "sharpe", rounds: int = 5, batch: int = 16,
             seed: int = 0, note: str = "", fixed: dict[str, float] | None = None,
             priority: int = jobs.PRIORITY_EXPERIMENT) -> dict[str, Any]:
    """`rounds` rounds of `batch` settings, each round proposed from every result before it, waited for in turn;
    then the experiment of every setting tried. fixed: other parameters held at these values. Its id, each
    round's, the best setting and its value."""
    if objective not in OBJECTIVES:
        raise ValueError(f"objective: one of {', '.join(OBJECTIVES)}")
    names, lower, upper, whole = _ranges(strategy, space)
    params = {p["name"]: p for p in strategy_classes()[strategy]}
    tried: list[list[float]] = []
    values: list[float | None] = []
    settings: list[dict[str, float]] = []
    rounds_run = []
    for r in range(rounds):
        out = numerics.run_engine_task("optimize", {"lower": lower, "upper": upper, "integer": whole,
                                                    "tried": tried, "values": values, "batch": batch,
                                                    "seed": seed * 1000 + r})
        if out.get("status") != "ok":
            raise ValueError((out.get("error") or {}).get("message", "the optimiser failed"))
        points = out["result_summary"]["points"]
        if not points:
            break  # the box is used up
        proposed = [{**(fixed or {}), **{n: within(n, x, params[n]) for n, x in zip(names, point)}}
                    for point in points]
        exp = ledger.submit(family=family, strategy=strategy, days=days, costs=costs, settings_list=proposed,
                            check=ROUND, priority=priority,
                            note=f"optimizer round {r + 1} of {rounds}: {objective}" + (f"; {note}" if note else ""))
        jobs.wait(ref=exp["experiment_id"])
        trials = {t["id"]: t for t in ledger.experiment(exp["experiment_id"])["trials"]}
        for point, setting, trial_id in zip(points, proposed, exp["trial_ids"]):
            tried.append([float(x) for x in point])
            values.append(_value(trials[trial_id]["summary"], objective))
            settings.append(setting)
        rounds_run.append(exp["experiment_id"])
    if not settings:
        raise ValueError("the optimiser proposed no setting")
    whole_run = ledger.submit(family=family, strategy=strategy, days=days, costs=costs, settings_list=settings,
                              priority=priority, note=f"optimizer: every setting of its {len(rounds_run)} rounds, "
                                                      f"{objective}" + (f"; {note}" if note else ""))
    jobs.wait(ref=whole_run["experiment_id"])
    scored = [(v, s) for v, s in zip(values, settings) if v is not None]
    best = max(scored, key=lambda vs: vs[0]) if scored else (None, None)
    return {"experiment_id": whole_run["experiment_id"], "rounds": rounds_run, "settings": len(settings),
            "objective": objective, "best": {"value": best[0], "setting": best[1]},
            "family_used": whole_run["family_used"], "family_budget": whole_run["family_budget"]}
