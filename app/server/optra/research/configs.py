"""Experiment configs: strategy/configs/<name>.json, each one backtest as the tool backtest takes it (the strategy, a
setting or a space of them, the family, the underlying and days, the costs, a note). Written and checked before it runs
(put_config), run by name (backtest with config), listed with the experiments that ran it (research_configs). Out of
git with the rest of strategy/ but demo: the owner's and the agents', like strategy/research.

A config is the whole request, defaults written out: what ran is the file, whatever the defaults become. Run again
unchanged, it finds its trials in the ledger and counts nothing new."""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any

from optra.research import ledger, space, strategies

NAME = re.compile(r"[a-z0-9][a-z0-9_]{0,63}")


def config_dir() -> Path:
    return strategies.strategy_dir() / "configs"


def path_of(name: str) -> Path:
    if not NAME.fullmatch(name):
        raise ValueError(f"config name {name!r}: lower-case letters, digits and _, at most 64")
    return config_dir() / f"{name}.json"


def read(name: str) -> dict[str, Any]:
    path = path_of(name)
    if not path.exists():
        raise FileNotFoundError(f"no strategy/configs/{name}.json")
    return json.loads(path.read_text())


def write(name: str, request: dict[str, Any]) -> Path:
    path = path_of(name)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(request, indent=2) + "\n")
    return path


def names() -> list[str]:
    return sorted(p.stem for p in config_dir().glob("*.json")) if config_dir().is_dir() else []


def settings_of(request: dict[str, Any]) -> list[dict[str, float]]:
    """The settings a config tries, each with every parameter (refused: an unknown class or parameter, a value out of
    its range)."""
    strategy = request["strategy"]
    parameters = strategies.strategy_classes().get(strategy)
    if parameters is None:
        raise strategies.unknown_strategy(strategy, strategies.built_strategies())
    if request.get("space"):
        search = {"kind": request.get("search", "grid"), "n": request.get("n", 0), "seed": request.get("seed", 0)}
        found = space.expand(strategy, request["space"], search)
    else:
        found = [request.get("setting") or {}]
    by_name = {p["name"]: p for p in parameters}
    out = []
    for s in found:
        s = space.resolved(strategy, s, parameters)
        out.append({k: space.within(k, v, by_name[k]) for k, v in s.items()})
    return out


def family_left(family: str) -> int | None:
    """Trials the family may still register; None for a family not yet in the ledger (it starts with the default)."""
    f = ledger.families().get(family)
    return None if f is None else f["budget"] - f["used"]


def runs(name: str) -> list[dict[str, Any]]:
    """The experiments that ran this config, oldest first: when, how many trials, and the best net among them."""
    out = []
    for e in ledger.experiments_of_config(name):
        nets = [(t["summary"] or {}).get("net") for t in ledger.experiment(e["id"])["trials"]]
        nets = [n for n in nets if n is not None]
        out.append({"experiment_id": e["id"], "created_at": e["created_at"], "trials": e["trials"],
                    "best_net": round(max(nets), 2) if nets else None})
    return out
