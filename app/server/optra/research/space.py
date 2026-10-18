"""The settings an experiment tries: a strategy's parameters (as entry_backtest --list-strategies declares them, with
their ranges; strategies.strategy_classes) and a space over them, expanded on a grid or sampled (random, Latin
hypercube). An optimiser would go here too, each of its steps a setting registered like any other."""

from __future__ import annotations

import itertools
import json
import math
import random
from typing import Any

from optra.research.strategies import strategy_classes


def canonical(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def resolved(strategy: str, setting: dict[str, float], parameters: list[dict[str, Any]] | None = None
             ) -> dict[str, float]:
    """The setting with every parameter of the class, its default where the setting has none: one setting, one
    form, so the same setting is the same trial, version and paper day however it was written."""
    params = parameters if parameters is not None else strategy_classes().get(strategy)
    if params is None:
        raise ValueError(f"unknown strategy {strategy}")
    names = {p["name"] for p in params}
    unknown = sorted(set(setting) - names)
    if unknown:
        raise ValueError(f"{strategy} has no parameter {', '.join(unknown)}")
    return {p["name"]: float(setting.get(p["name"], p["default"])) for p in params}


def _values(name: str, spec: Any, param: dict[str, Any]) -> list[float] | tuple[float, float]:
    """A grid's values, or a range (lo, hi) to sample from."""
    if isinstance(spec, (int, float)):
        return [float(spec)]
    if isinstance(spec, list):
        return [float(v) for v in spec]
    if isinstance(spec, dict) and "values" in spec:
        return [float(v) for v in spec["values"]]
    if isinstance(spec, dict) and "range" in spec:
        lo, hi = (float(v) for v in spec["range"])
        if lo > hi:
            raise ValueError(f"{name}: range low above high")
        if "step" in spec:
            step = float(spec["step"])
            if step <= 0:
                raise ValueError(f"{name}: step must be positive")
            n = int(math.floor((hi - lo) / step + 1e-9)) + 1
            return [lo + i * step for i in range(n)]
        return (lo, hi)
    raise ValueError(f"{name}: a number, a list, {{'values': [...]}} or {{'range': [lo, hi], 'step'?}}")


def within(name: str, value: float, param: dict[str, Any]) -> float:
    if param["integer"]:
        value = float(round(value))
    if not param["min"] <= value <= param["max"]:
        raise ValueError(f"{name}={value:g} outside [{param['min']:g}, {param['max']:g}]")
    return value


def expand(strategy: str, space: dict[str, Any], search: dict[str, Any]) -> list[dict[str, float]]:
    """The settings an experiment tries: every combination of the space's values (search "grid"), or n
    samples ("random", "latin": a Latin hypercube, each range cut into n strata sampled once), with a seed.
    Parameters not in the space keep their defaults."""
    params = strategy_classes().get(strategy)
    if params is None:
        raise ValueError(f"unknown strategy {strategy}")
    by_name = {p["name"]: p for p in params}
    unknown = sorted(set(space) - set(by_name))
    if unknown:
        raise ValueError(f"{strategy} has no parameter {', '.join(unknown)}")
    kind = search.get("kind", "grid")
    values = {name: _values(name, spec, by_name[name]) for name, spec in space.items()}
    names = list(values)
    if kind == "grid":
        ranges = [n for n in names if isinstance(values[n], tuple)]
        if ranges:
            raise ValueError(f"a grid needs values, not ranges: {', '.join(ranges)} (give a step)")
        combos = [dict(zip(names, combo)) for combo in itertools.product(*(values[n] for n in names))]
    elif kind in ("random", "latin"):
        n = int(search.get("n", 0))
        if n <= 0:
            raise ValueError(f"search {kind} needs n > 0")
        rng = random.Random(search.get("seed", 0))
        columns: dict[str, list[float]] = {}
        for name in names:
            v = values[name]
            if isinstance(v, tuple):
                lo, hi = v
                if kind == "latin":
                    column = [lo + (hi - lo) * (i + rng.random()) / n for i in range(n)]
                    rng.shuffle(column)
                else:
                    column = [rng.uniform(lo, hi) for _ in range(n)]
            else:
                if kind == "latin":
                    column = [v[(i * len(v)) // n] for i in range(n)]
                    rng.shuffle(column)
                else:
                    column = [rng.choice(v) for _ in range(n)]
            columns[name] = column
        combos = [{name: columns[name][i] for name in names} for i in range(n)]
    else:
        raise ValueError(f"unknown search {kind}: grid, random or latin")
    settings_ = []
    seen = set()
    for combo in combos:
        checked = {name: within(name, value, by_name[name]) for name, value in combo.items()}
        key = canonical(checked)
        if key not in seen:
            seen.add(key)
            settings_.append(checked)
    return settings_
