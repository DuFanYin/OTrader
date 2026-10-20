"""What an experiment's results say once the tries are counted (every trial the ledger had registered by then). The statistics over the matrix of trials by
days are the numerics library's ("selection", engine/quant/stats/selection.cpp): the deflated Sharpe ratio of the best
trial, the probability of backtest overfitting, a walk-forward. Here: the best trial without its best days and by the
underlying's move, and the robustness checks that need new runs, queued as experiments of their own."""

from __future__ import annotations

import contextlib
import copy
import math
from collections import OrderedDict
from pathlib import Path
from typing import Any, Iterable, Sequence

from optra.data import chains
from optra.data.days import day_of
from optra.infra import numerics
from optra.research import jobs, ledger
from optra.research.space import within
from optra.research.strategies import strategy_classes

# Each analysis, by what it is computed from: the experiment, its finished trials (a finished trial's results never
# change), the tries counted, the windows asked for, and the library's build; the ones used last kept, up to
# ANALYSES_KEPT.
_analyses: OrderedDict[tuple, dict[str, Any]] = OrderedDict()
ANALYSES_KEPT = 256


def without_best_days(row: Iterable[float], n: int = 5) -> dict[str, Any]:
    x = sorted(float(v) for v in row if not math.isnan(v))
    kept = x[:-n] if len(x) > n else []
    return {"days_dropped": n, "net": sum(kept), "net_with_them": sum(x), "days": len(kept)}


def by_regime(row: Sequence[float], days: list[str], moves: dict[str, float]) -> dict[str, Any]:
    """The trial's days split by the underlying's move that day: up and down, and the quietest half
    against the wildest."""
    pairs = [(moves[d], float(row[i])) for i, d in enumerate(days) if d in moves and not math.isnan(row[i])]
    if len(pairs) < 4:
        return {"why": "too few days with the underlying's move"}
    size = sorted(abs(m) for m, _ in pairs)[len(pairs) // 2]

    def part(selected: list[float]) -> dict[str, Any]:
        return {"days": len(selected), "net": sum(selected),
                "mean": sum(selected) / len(selected) if selected else 0.0}

    return {"up": part([v for m, v in pairs if m > 0]), "down": part([v for m, v in pairs if m <= 0]),
            "quiet": part([v for m, v in pairs if abs(m) <= size]),
            "wild": part([v for m, v in pairs if abs(m) > size])}


def analysis(experiment_id: str, train: int | None = None, test: int | None = None) -> dict[str, Any]:
    """What the experiment's results say once the tries are counted (ledger.tries_by), computed by the numerics
    library (engine/quant, "selection"): the deflated Sharpe ratio of its best trial, the probability of
    backtest overfitting, a walk-forward; and the best trial without its best days and by the
    underlying's move."""
    exp = ledger.experiment(experiment_id)
    ids, days, values = ledger.matrix(experiment_id)
    if not ids:
        return {"experiment_id": experiment_id, "why": "no trial has finished"}
    tries = max(ledger.tries_by(experiment_id), len(ids))
    key = (experiment_id, len(ids), tries, train or 0, test or 0, numerics.library_build())
    if (kept := _analyses.get(key)) is not None:
        with contextlib.suppress(KeyError):  # dropped by another request since
            _analyses.move_to_end(key)
        return copy.deepcopy(kept)
    try:
        stats = numerics.selection(values, tries, train or 0, test or 0)
    except numerics.EngineError as exc:
        raise ValueError(str(exc)) from exc
    best = stats["deflated_sharpe"]["best"]
    for w in stats["walk_forward"]["windows"]:
        w["trial_id"] = ids[w["best"]]
        last = min(len(days) - 1, w["test_first"] + stats["walk_forward"]["test"] - 1)
        w["test_days"] = [days[w["test_first"]], days[last]]
    trial = next(t for t in exp["trials"] if t["id"] == ids[best])
    out = {
        "experiment_id": experiment_id, "trials": len(ids), "days": len(days), "family": exp["family"],
        "tries": tries, "family_tries": exp["family_counts"]["used"] if exp["family_counts"] else len(ids),
        "best": {"id": trial["id"], "setting": trial["setting"], "summary": trial["summary"]},
        "deflated_sharpe": stats["deflated_sharpe"], "pbo": stats["pbo"], "walk_forward": stats["walk_forward"],
        "without_best_days": without_best_days(values[best]),
        "by_regime": by_regime(values[best], days, ledger.day_moves(days)),
    }
    _analyses[key] = copy.deepcopy(out)
    while len(_analyses) > ANALYSES_KEPT:
        with contextlib.suppress(KeyError):
            _analyses.popitem(last=False)
    return out


# A setting found on one underlying, run on the other over the same dates: SPY's and the S&P 500's options.
OTHER_UNDERLYING = {"SPY": "SPXW", "SPXW": "SPY", "XSP": "SPY"}


def other_underlying_days(days: list[Path]) -> tuple[str, list[Path]] | None:
    """The other underlying's backtest days (OTHER_UNDERLYING) of the same dates, its holdout left out; None for an
    underlying with no other, or when it has none of the dates."""
    other = OTHER_UNDERLYING.get(days[0].parent.parent.name) if days else None
    if other is None:
        return None
    have = chains.day_files(other)
    cut = ledger.holdout_from(other)
    found = []
    for f in days:
        d = day_of(f)
        if d is not None and (cut is None or d < cut) and (hit := have.get(d.isoformat())) and hit[1] == "backtest":
            found.append(hit[0])
    return (other, found) if found else None


def robustness(experiment_id: str, trial_id: str | None = None,
               priority: int = jobs.PRIORITY_EXPERIMENT) -> dict[str, Any]:
    """The checks that need new runs, on a trial (the best by Sharpe unless named), as experiments of their
    own under the same family: its costs doubled (fees and the fill positions); its fills held to the size shown and a
    minute late; every leg at its bid or ask, when it filled inside the spread; and each parameter it varied moved 20%
    down and up, one at a time; and the same setting on the other underlying over the same dates (SPXW's days for one
    found on SPY, SPY's for SPXW and XSP: other_underlying_days), reported but not a gate. They count against the
    family's budget like any trial."""
    exp = ledger.experiment(experiment_id)
    done = [t for t in exp["trials"] if t["status"] == "done"]
    if trial_id is None:
        if not done:
            raise ValueError("no trial has finished")
        trial_id = max(done, key=lambda t: t["summary"]["sharpe"])["id"]
    trial = next((t for t in exp["trials"] if t["id"] == trial_id), None)
    if trial is None:
        raise KeyError(trial_id)
    spec = exp["spec"]
    common = {"family": exp["family"], "strategy": exp["strategy"],
              "days": [str(f) for f in ledger.experiment_days(experiment_id)],
              "risk_free_rate": spec["pricing"]["risk_free_rate"], "iv_price_mode": spec["pricing"]["iv_price_mode"],
              "parent": experiment_id, "priority": priority}
    costs = ledger.Costs.of(spec["costs"])
    checks: dict[str, dict[str, Any]] = {}  # submitted together: those on the same days run as one batch
    checks["costs_doubled"] = dict(**common, costs=costs.doubled(), settings_list=[trial["setting"]],
                                   note=f"fees and the fill positions doubled for {trial_id}")
    checks["fills_shown_size_late"] = dict(**common, costs=costs.strict(), settings_list=[trial["setting"]],
                                           note=f"fills of the size shown, a minute late, for {trial_id}")
    if quoted := costs.at_the_quotes():
        checks["fills_at_the_quotes"] = dict(**common, costs=quoted, settings_list=[trial["setting"]],
                                             note=f"every leg at its bid or ask, for {trial_id}")
    params = {p["name"]: p for p in strategy_classes()[exp["strategy"]]}
    varied = sorted((spec.get("space") or {}).keys()) or sorted(trial["setting"])
    neighbours = []
    for name in varied:
        value = trial["setting"].get(name)
        if value is None or value == 0:
            continue
        for factor in (0.8, 1.2):
            moved = within(name, max(params[name]["min"], min(params[name]["max"], value * factor)), params[name])
            if moved != value:
                neighbours.append({**trial["setting"], name: moved})
    if neighbours:
        checks["parameters_moved"] = dict(**common, costs=costs, settings_list=neighbours,
                                          note=f"each parameter of {trial_id} moved 20% down and up")
    if other := other_underlying_days(ledger.experiment_days(experiment_id)):
        checks["other_underlying"] = dict(**(common | {"days": [str(f) for f in other[1]]}), costs=costs,
                                          settings_list=[trial["setting"]],
                                          note=f"{trial_id} on {other[0]}, the same dates")
    submitted = dict(zip(checks, ledger.submit_together([e | {"check": k} for k, e in checks.items()])))
    return {"trial_id": trial_id} | {k: submitted.get(k) for k in (
        "costs_doubled", "fills_shown_size_late", "fills_at_the_quotes", "parameters_moved", "other_underlying")}
