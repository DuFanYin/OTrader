"""A combo from the screener as ComboStrategy: the legs as rules, the engine's tools in the tool list, and (when entry_backtest is built) a combo backtested and
hedge-studied on synthetic days."""

from __future__ import annotations

import json
import subprocess
from datetime import date

import pytest

from optra.utils import files
from optra.tools import TOOLS, engine  # noqa: E402

from optra.data import synthetic_days  # noqa: E402
from optra.research import batch, combo  # noqa: E402
from optra.utils.files import repo_root  # noqa: E402

ENTRY = repo_root() / "engine" / "build" / "entry_backtest"


def _has_combo_strategy() -> bool:
    if not ENTRY.is_file():
        return False
    out = subprocess.run([str(ENTRY), "--list-strategies"], capture_output=True, text=True, timeout=30)
    return any(s["name"] == combo.CLASS for s in json.loads(out.stdout).get("strategies", []))


def test_a_screener_leg_becomes_the_rule_that_picks_it():
    leg = {"option_type": "put", "qty": -1, "strike": 594.0, "expiry": "2025-03-07", "forward": 600.0}
    assert combo.leg_rule(leg, date(2025, 3, 3)) == {"right": 1, "ratio": -1, "strike_pct": -1.0, "dte": 4}
    assert combo.leg_rule({"option_type": "call", "ratio": 2, "strike_pct": 0.5, "dte": 0}, None) == {
        "right": 0, "ratio": 2, "strike_pct": 0.5, "dte": 0}
    with pytest.raises(ValueError):
        combo.leg_rule({"option_type": "call", "ratio": 1, "strike": 600.0}, None)  # no forward, no as_of
    values = combo.setting([{"option_type": "call", "ratio": 1, "strike_pct": 0}], None, quantity=2)
    assert values["leg1_ratio"] == 1 and values["leg4_ratio"] == 0 and values["quantity"] == 2


def test_the_engine_tools_are_in_the_tool_list():
    assert {t.name for t in engine.TOOLS} == {"backtest", "put_config", "research_configs", "backtest_days",
                                              "cache_backtest_days", "backtest_report",
                                              "backtest_combo",
                                              "hedge_study",
                                              "holdings_stress"}
    assert all(TOOLS[t.name] is t for t in engine.TOOLS)


@pytest.fixture()
def days(tmp_path, monkeypatch):
    model = synthetic_days.Model(implied_vol=0.2)
    synthetic_days.generate(model, date(2025, 3, 3), date(2025, 3, 5), tmp_path, "SPY", False)
    monkeypatch.setattr(files, "data_root", lambda: tmp_path)
    return tmp_path


@pytest.mark.skipif(not _has_combo_strategy(), reason="entry_backtest with ComboStrategy not built")
def test_a_straddle_backtested_and_hedge_studied(days):
    straddle = engine.BacktestComboRequest(symbol="SPY", legs=[
        {"option_type": "call", "ratio": 1, "strike_pct": 0, "dte": 0},
        {"option_type": "put", "ratio": 1, "strike_pct": 0, "dte": 0}])
    r = engine.backtest_combo(straddle)
    assert r.summary["days"] == 3 and r.summary["trades"] == 3
    traded = r.details["trades"]
    for day in traded:  # the sources make the P&L before fees, which is the net plus them
        assert sum(day["attribution"].values()) >= day["net"] - 0.05
    assert r.details["strategy"]["strategy_class"] == "ComboStrategy"
    assert r.details["strategy"]["portfolio_name"] == "SPY"

    study = engine.hedge_study(engine.HedgeStudyRequest(symbol="SPY", legs=straddle.legs, delta_ranges=[20]))
    rows = study.details["rows"]
    assert [row["variant"] for row in rows] == ["unhedged", "within 20 shares"]
    assert rows[0]["hedge_fills"] == 0 and rows[1]["hedge_fills"] > 0
    assert study.summary["least_tail_loss"] in ("unhedged", "within 20 shares")
