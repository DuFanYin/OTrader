"""What the system recorded about strategy versions, which only it writes: each paper day of a version's strategy
against a backtest of the day (the daily report, services.session). research.promotion reads them for its gates; a
look at a holdout is the experiment whose days reach it (ledger: holdout)."""

from __future__ import annotations

import json
from typing import Any

from optra.research.space import canonical, resolved
from optra.research.store import db, lock as _lock


def record_paper_day(strategy_name: str, strategy: str, setting: dict[str, float], day: str, paper_net: float,
                     backtest_net: float, paper_fills: int, backtest_fills: int, unmatched: int,
                     code: dict[str, str], backtest_slippage: float | None = None,
                     paper_slippage: float | None = None) -> None:
    """A paper day of a live strategy against a backtest of the same recorded day (the daily report's comparison).
    code: the trading core the paper engine reported ("engine") and the strategy library it ran ("strategy");
    backtest_slippage: USD the backtest's fills paid over the mids, paper_slippage the paper fills' (session.compare)."""
    setting = resolved(strategy, setting)
    with _lock:
        db().execute("INSERT OR REPLACE INTO paper_days VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                     (strategy_name, strategy, canonical(setting), day, paper_net, backtest_net, paper_fills,
                      backtest_fills, unmatched, canonical(code), backtest_slippage, paper_slippage))


def paper_days(strategy: str, setting: dict[str, float], code: dict[str, str]) -> list[dict[str, Any]]:
    """The paper days of every live strategy running this class with this setting, by day, on this code (the
    version's engine and strategy): a day of other code is not this version's."""
    rows = db().execute("SELECT * FROM paper_days WHERE strategy = ? AND setting = ? ORDER BY day",
                        (strategy, canonical(setting))).fetchall()
    days = [{**dict(r), "code": json.loads(r["code"])} for r in rows]
    return [d for d in days if d["code"].get("engine") == code.get("engine")
            and d["code"].get("strategy") == code.get("strategy")]
