"""Experiment configs (strategy/configs): written with every default, checked before they run, run by name, found again
in the ledger; the ComboStrategy settings they lean on and the round trips the engine cuts, on synthetic days through
entry_backtest."""

from __future__ import annotations

import json
import subprocess
from datetime import date
from pathlib import Path

import pytest

from optra.research import configs, ledger, runner, store
from optra.data import daily_values, synthetic_days
from optra.tools import engine
from optra.tools.base import ToolError
from optra.utils import files
from optra.utils.files import repo_root

ENTRY = repo_root() / "engine" / "build" / "entry_backtest"
STRADDLE = {"leg1_ratio": 1, "leg2_ratio": 1, "leg2_right": 1, "entry_minute": 30, "exit_minute": 60}


@pytest.fixture
def week(tmp_path, monkeypatch):
    """Synthetic SPY days Wednesday to Friday (an expiry every day), a ledger and a config folder of their own."""
    synthetic_days.generate(synthetic_days.Model(implied_vol=0.2), date(2025, 3, 5), date(2025, 3, 7), tmp_path,
                            "SPY", False)
    monkeypatch.setattr(files, "data_root", lambda: tmp_path)
    monkeypatch.setattr(store, "_db", None)
    monkeypatch.setattr(configs, "config_dir", lambda: tmp_path / "configs")
    return sorted(str(p) for p in (tmp_path / "SPY").rglob("*.parquet"))


def put(name: str, **request) -> engine.Result:
    return engine.put_config(engine.PutConfigRequest(name=name, request=engine.BacktestRequest(**request)))


def test_a_config_is_written_whole_and_checked_before_it_runs(week):
    out = put("straddles", strategy="ComboStrategy", space={**STRADDLE, "entry_minute": [30, 60]},
              family="straddles", symbol="SPY")
    assert out.summary["settings"] == 2 and out.summary["family_trials_left"] is None  # a family not yet in the ledger
    written = json.loads((configs.config_dir() / "straddles.json").read_text())
    assert written["costs"] == engine.Costs().model_dump() and "config" not in written  # every default, written out
    assert ledger.families() == {}  # checked, not run: nothing registered
    with pytest.raises(ToolError, match="no parameter leg9_ratio"):
        put("bad", strategy="ComboStrategy", setting={"leg9_ratio": 1}, symbol="SPY")
    with pytest.raises(ToolError, match="outside"):
        put("bad", strategy="ComboStrategy", setting={"entry_minute": 999}, symbol="SPY")
    with pytest.raises(ToolError, match="config name"):
        put("Bad Name", strategy="ComboStrategy", symbol="SPY")
    with pytest.raises(ToolError, match="no other config"):
        put("bad", config="straddles")
    assert configs.names() == ["straddles"]


def test_a_config_runs_by_name_alone_and_its_runs_are_found(week):
    put("one_straddle", strategy="ComboStrategy", setting=STRADDLE, family="straddles", symbol="SPY")
    with pytest.raises(ToolError, match="config alone"):
        engine.backtest(engine.BacktestRequest(config="one_straddle", symbol="SPXW"))
    with pytest.raises(ToolError, match="no strategy/configs/missing.json"):
        engine.backtest(engine.BacktestRequest(config="missing"))
    first = engine.backtest(engine.BacktestRequest(config="one_straddle"))
    assert first.summary["config"] == "one_straddle" and first.summary["days"] == len(week)
    again = engine.backtest(engine.BacktestRequest(config="one_straddle"))
    assert ledger.families()["straddles"]["used"] == 1  # the same trial: counted once
    listed = engine.research_configs(engine.NoInput()).details["configs"]
    assert [c["config"] for c in listed] == ["one_straddle"] and listed[0]["settings"] == 1
    assert [r["experiment_id"] for r in listed[0]["runs"]] == [first.summary["experiment_id"],
                                                               again.summary["experiment_id"]]
    (configs.config_dir() / "broken.json").write_text("{not json")
    broken = next(c for c in engine.research_configs(engine.NoInput()).details["configs"] if c["config"] == "broken")
    assert broken["problem"] and broken["runs"] == []


def _run(days: list[str], daily: Path | None = None, **setting) -> dict:
    """ComboStrategy on these days (with the values by day in `daily`): entry_backtest's answer, the trial's line
    (runner.read)."""
    args = [f"{k}={v}" for k, v in setting.items()] + (["--daily", str(daily)] if daily else [])
    out = subprocess.run([str(ENTRY), "--files", *days, "ComboStrategy", *args], capture_output=True,
                         timeout=120, check=True)
    return next(runner.read(out.stdout)[1])


def _combo(days: list[str], daily: Path | None = None, **setting) -> list[list[dict]]:
    """Each day's fills."""
    out = _run(days, daily, **setting)
    fills = runner.rows(out["fills"])
    return [[f for f in fills if f["date"] == on] for on in out["days"]["date"]]


needs_engine = pytest.mark.skipif(not ENTRY.is_file(), reason="entry_backtest not built")


@needs_engine
def test_exact_dte_skips_a_day_without_that_expiry(week):
    # Friday's next expiry is Monday, three days out: with exact_dte no combo, without it Monday's
    exact = _combo(week, leg1_ratio=1, leg1_dte=1, exact_dte=1, entry_minute=30, exit_minute=60)
    loose = _combo(week, leg1_ratio=1, leg1_dte=1, entry_minute=30, exit_minute=60)
    assert [len(d) for d in exact] == [2, 2, 0]
    assert loose[2] and loose[2][0]["symbol"].startswith("SPY-20250310-C")


@needs_engine
def test_close_itm_only_closes_the_legs_in_the_money_and_leaves_the_rest_to_expire(week):
    fly = {"leg1_ratio": -1, "leg2_ratio": -1, "leg2_right": 1, "leg3_ratio": 1, "leg3_strike_pct": 1,
           "leg4_ratio": 1, "leg4_right": 1, "leg4_strike_pct": -1, "entry_minute": 150, "exit_minute": 385}
    for day in _combo(week, **fly, close_itm_only=1):
        exits = [f for f in day if f["time"].endswith("20:56:00")]  # 15:56 New York
        assert len(exits) == 1  # one of the two legs sold at the money, never a wing
        settled = [f for f in day if f["time"].endswith("21:00:00") and f["symbol"] != "SPY"]
        assert len(settled) == 3
    for day in _combo(week, **fly):
        assert len([f for f in day if f["time"].endswith("20:56:00")]) == 4


@needs_engine
def test_a_bought_leg_needs_only_an_ask(week):
    # a call 1.5% out at 15:30 has no bid, only an ask: it is bought all the same
    for day in _combo(week, leg1_ratio=1, leg1_strike_pct=1.5, entry_minute=360, exit_minute=385):
        assert day and day[0]["side"] == "BUY" and day[0]["price"] > 0


@needs_engine
def test_the_entry_filters(week):
    # the day before's realized vol, from the values by day: unknown on the first day, no combo; known after
    daily = daily_values.add_minute_rv([Path(d) for d in week], "SPY").parent
    assert sorted(daily_values.read_series(daily / "SPY_minute_rv.csv")) == ["2025-03-05", "2025-03-06", "2025-03-07"]
    assert [len(d) for d in _combo(week, daily, **STRADDLE, prev_rv_max=5)] == [0, 4, 4]
    assert [len(d) for d in _combo(week, daily, **STRADDLE, prev_rv_min=5)] == [0, 0, 0]  # never that wild
    assert [len(d) for d in _combo(week, **STRADDLE, prev_rv_max=5)] == [0, 0, 0]  # no values by day: never known
    assert [len(d) for d in _combo(week, **STRADDLE, move_max=0.0001)] == [0, 0, 0]


@needs_engine
def test_a_round_trip_ends_at_the_bar_the_position_is_flat_again(week):
    from optra.research import batch

    # a butterfly held into exercise: shares delivered at the close, sold the next morning; a new one that afternoon
    out = _run(week, leg1_ratio=1, leg1_strike_pct=-0.5, leg2_ratio=-2, leg3_ratio=1, leg3_strike_pct=0.5,
               entry_minute=360, hold=1)
    trips = runner.rows(out["round_trips"])
    assert len(trips) == 3 and trips[-1]["closed"] is None  # the last still open at the run's end
    for trip in trips[:-1]:
        assert trip["days"] == 2 and trip["closed"].endswith("14:32:00")  # 09:32 New York, the shares sold
        assert trip["entry_cost"] > 0 and trip["max_loss"] > 0
    assert sum(t["net"] for t in trips) == pytest.approx(sum(out["days"]["net_pnl"]))
    trades, errors, notes = batch.trades_of(out)
    assert len(trades) == 2 and trades[0].risk == trips[0]["max_loss"]
    assert not errors and any("still open" in n for n in notes)  # a note: the run went through its days
    # a credit taken with an unbounded loss has no R: left out, said so
    naked = batch.trades_of(_run(week, leg1_ratio=-1, entry_minute=30, exit_minute=60))
    assert naked[0] == [] and any("no bounded loss" in n for n in naked[2])
