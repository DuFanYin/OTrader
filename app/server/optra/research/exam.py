"""The research process examined on synthetic days whose answer is known: not a strategy's test, the process's. Worlds
of one model (data.synthetic_days), each in a data folder and a ledger of its own: noise, where the underlying is
a random walk and the options are quoted at fair value, so no strategy has an edge; and planted, the same days with
intraday momentum (the last 30 minutes drift by `momentum` times the first 30 minutes' return), the edge
SyntheticMomentumStrategy (strategy/demo) is made for; each day lists the 0DTE expiry only, 10 strikes either side.
The same sweep goes through the agent's backtest in every world. The process passes when, in every noise world, the
deflated Sharpe holds the best back, and in the planted one the best beats the control and clears it.

    uv run --project app/server python -m optra.research.exam [--days 250] [--noise-worlds 5] [--momentum 1.0]
"""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
import time
from datetime import date, timedelta
from pathlib import Path
from typing import Any

from optra.research import backtest, ledger, promotion, store
from optra.data import synthetic_days
from optra.utils import files

STRATEGY = "SyntheticMomentumStrategy"
SPACE = {"signal_minutes": [15, 30, 60], "entry_minute": [300, 330, 360], "exit_minute": [380, 389]}
DAY = {"expiries": 1, "strikes": 10}  # what the strategy buys, no more: the days are made quickly
COSTS = ledger.Costs()
FIRST_DAY = date(2023, 1, 2)  # before any holdout


def world(name: str, model: synthetic_days.Model, days: int) -> dict[str, Any]:
    """The sweep through backtest on `days` weekdays of the model, in a data folder and a ledger of its own."""
    end = FIRST_DAY + timedelta(days=days * 7 // 5 + 3)
    data_root, db = files.data_root, store._db
    with tempfile.TemporaryDirectory(prefix=f"optra-exam-{name}-") as tmp:
        root = Path(tmp)
        synthetic_days.generate(model, FIRST_DAY, end, root / "backtest", "SPY", False)
        last = sorted(p.stem for p in (root / "backtest" / "SPY").rglob("*.parquet"))[days - 1]
        files.data_root, store._db = (lambda: root), None
        try:
            started = time.monotonic()
            out = backtest.backtest(strategy=STRATEGY, setting=None, space=SPACE, search={"kind": "grid"},
                                    family=f"exam {name}", symbol="SPY", start=FIRST_DAY,
                                    end=date(int(last[:4]), int(last[4:6]), int(last[6:])), costs=COSTS)
        finally:
            if store._db is not None:
                store._db.close()
            files.data_root, store._db = data_root, db
    dsr = out["deflated_sharpe"]
    return {"world": name, "momentum": model.momentum, "seed": model.seed, "days": out["days"],
            "tries": out["tries"], "best": out["best"]["setting"], "best_trades": out["best"].get("trades"),
            "best_net_usd": out["best"].get("net USD"),
            "best_expectancy_r": out["best"].get("expectancy_r"), "control_expectancy_r": out["control"].get(
                "expectancy_r"), "beats_control_by_r": out["beats_control_by_r"], "deflated_sharpe": dsr,
            "overfitting_probability": out["overfitting_probability"], "walk_forward_net": out["walk_forward_net"],
            "passes_dsr": dsr is not None and dsr > promotion.DSR_BAR, "seconds": round(time.monotonic() - started)}


def exam(days: int = 250, noise_worlds: int = 5, momentum: float = 1.0, seed: int = 1) -> dict[str, Any]:
    noise = [world(f"noise {s}", synthetic_days.Model(seed=s, **DAY), days) for s in range(seed, seed + noise_worlds)]
    planted = world("planted", synthetic_days.Model(momentum=momentum, seed=seed, **DAY), days)
    held_back = [not w["passes_dsr"] for w in noise]
    found = planted["passes_dsr"] and (planted["beats_control_by_r"] or 0) > 0
    return {"passed": all(held_back) and found, "noise_held_back": f"{sum(held_back)} of {len(noise)}",
            "planted_found": found, "worlds": [*noise, planted]}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--days", type=int, default=250, help="weekdays in each world")
    ap.add_argument("--noise-worlds", type=int, default=5, help="noise worlds, each with its own seed")
    ap.add_argument("--momentum", type=float, default=1.0, help="the planted world's momentum")
    ap.add_argument("--seed", type=int, default=1, help="the first seed")
    a = ap.parse_args(argv)
    out = exam(a.days, a.noise_worlds, a.momentum, a.seed)
    print(json.dumps(out, indent=2))
    return 0 if out["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
