"""The Backtest view's sample (app/web/public/samples/sample_backtest.json), made on synthetic days so the repository
ships no result of a data vendor's data: an at-the-money straddle bought on its expiry day, a week of SPY days from the
synthetic model, run as the page runs a backtest (services.backtest), saved as the view saves a sample.

    uv run --project app/server python scripts/backtest_sample.py
"""

from __future__ import annotations

import json
import sys
import tempfile
import time
from datetime import date
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "app" / "server"))  # the optra package
from optra.research import jobs  # noqa: E402
from optra.services import backtest  # noqa: E402
from optra.data import synthetic_days  # noqa: E402
from optra.utils import files  # noqa: E402

OUT = Path(__file__).resolve().parents[1] / "app" / "web" / "public" / "samples" / "sample_backtest.json"
START, END = date(2025, 8, 4), date(2025, 8, 8)
SETTING = {"leg1_ratio": 1, "leg2_ratio": 1, "leg2_right": 1}
COSTS = {"fee_scale": 1.0}


def main() -> None:
    with tempfile.TemporaryDirectory() as data:
        root = Path(data)
        synthetic_days.generate(synthetic_days.Model(seed=1), START, END, root / "backtest", "SPY", False)
        files.data_root = lambda: root  # the page's backtest reads its days from data/backtest/
        jobs.use_workers(False)
        started = time.monotonic()
        out = backtest.run_backtest({"parquet": "SPY", "strategy": "ComboStrategy", "start_date": START.isoformat(),
                                     "end_date": END.isoformat(), "costs": COSTS, "risk_free_rate": 0.05,
                                     "iv_price_mode": "mid", "strategy_setting": SETTING})
        wall = time.monotonic() - started
    if out.get("status") != "ok":
        raise SystemExit(f"the backtest failed: {out.get('error')}")
    out["day_files"] = [Path(f).name for f in out.get("day_files", [])]  # not the temporary folder
    inputs = {"symbol": "SPY", "strategy": "ComboStrategy", "start": START.isoformat(), "end": END.isoformat(),
              "changed": SETTING, "costs": COSTS, "rate": 0.05, "ivMode": "mid"}
    OUT.write_text(json.dumps({"state": "ok", "inputs": inputs, "data": out, "wall": wall}))
    print(f"{OUT}: {out['summary']}")


if __name__ == "__main__":
    main()
