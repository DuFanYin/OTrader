"""Hedging positions, the run history, the Treasury curve's cache and the scenario sweep.

Run:  server/.venv/bin/python -m pytest tests/test_hedging_and_history.py   (offline)
"""

from __future__ import annotations

import asyncio
import json
import os
import tempfile
import time
from pathlib import Path

import pytest  # noqa: E402
from fastapi.testclient import TestClient  # noqa: E402

from main import create_app  # noqa: E402
from optra.tools import ToolError, call_sync  # noqa: E402
from optra.infra import numerics, run_store  # noqa: E402
from optra.services import market_data  # noqa: E402

APP = create_app(engine=False)  # the server without the live engine

OPTION = {"spot": 100.0, "strike": 100.0, "rate": 0.03, "vol": 0.2, "maturity": 0.5, "n_paths": 4000}


def hedge(**kw):
    r = call_sync("compare_hedges", **{**OPTION, **kw})
    return r["result_summary"], r["result_details"]["strategy_compare"]


# ── Hedging ───────────────────────────────────────────────────────────────────

def test_a_fairly_priced_option_hedges_to_zero_and_costs_lose():
    s, by = hedge()
    assert s["n_legs"] == 1 and abs(s["pnl_mean"]) < 0.1 and s["best_strategy"] == "discrete_delta"
    assert by["with_transaction_cost"]["mean"] < by["discrete_delta"]["mean"] - 0.05
    assert by["with_transaction_cost"]["transaction_cost"] > 0 and by["discrete_delta"]["transaction_cost"] == 0
    assert by["with_transaction_cost"]["es95"] > by["discrete_delta"]["es95"]  # costs make the worst paths worse


def test_hedging_does_not_depend_on_threads():
    prev = numerics.get_max_threads()
    try:
        numerics.set_num_threads(1)
        one = hedge()[0]
        numerics.set_num_threads(8)
        eight = hedge()[0]
    finally:
        numerics.set_num_threads(prev)
    assert one == eight


def test_positions():
    default = hedge()[0]
    sold_call = hedge(legs=[{"option_type": "call", "strike": 100.0, "quantity": -1}])[0]
    assert sold_call["pnl_std"] == default["pnl_std"]
    sold_put = hedge(option_type="put")[0]
    assert sold_put == hedge(legs=[{"option_type": "put", "strike": 100.0, "quantity": -1}])[0]
    # holding a position is the mirror of selling it: without costs, every path's P&L flips sign
    long = hedge(legs=[{"option_type": "call", "strike": 100.0, "quantity": 1}], transaction_cost_bps=0)[1]["discrete_delta"]
    short = hedge(legs=[{"option_type": "call", "strike": 100.0, "quantity": -1}], transaction_cost_bps=0)[1]["discrete_delta"]
    assert long["mean"] == pytest.approx(-short["mean"], abs=1e-9) and long["std"] == pytest.approx(short["std"])
    # a calendar: each leg settles at its own expiry; the horizon is the longest
    cal = hedge(legs=[{"option_type": "call", "strike": 100.0, "quantity": 1, "maturity": 1.0},
                      {"option_type": "call", "strike": 100.0, "quantity": -1, "maturity": 0.25}])[0]
    assert cal["n_legs"] == 2 and cal["horizon"] == 1.0
    # bought above the vol the market moves with: the hedged holder loses the difference
    rich = hedge(legs=[{"option_type": "call", "strike": 100.0, "quantity": 1, "vol": 0.3}], transaction_cost_bps=0)[0]
    assert rich["pnl_mean"] < -1.0


def test_portfolio_stress_hedges_the_position():
    legs = [{"option_type": "call", "strike": 110.0, "quantity": 1}, {"option_type": "put", "strike": 90.0, "quantity": 1}]
    r = call_sync("stress_test", **{k: OPTION[k] for k in ("spot", "strike", "rate", "vol", "maturity")}, legs=legs, hedge_paths=500)
    rows = r["result_details"]["rows"]
    assert r["result_summary"]["hedge_compare"] and all(row["hedge"]["es95"] is not None and row["binomial"] for row in rows)


# ── Scenario sweep ────────────────────────────────────────────────────────────

def test_scenario_sweep_prices_puts_and_dividends():
    for option_type in ("call", "put"):
        for q in (0.0, 0.03):
            args = {**OPTION, "strike": 105.0, "option_type": option_type, "dividend_yield": q}
            sweep = call_sync("scenario_sweep", **args)
            assert sweep["warnings"] == []
            assert sweep["result_summary"]["base_price"] == pytest.approx(call_sync("price_option", **args)["result_summary"]["bs"], rel=1e-12)


# ── Run history ───────────────────────────────────────────────────────────────

@pytest.fixture()
def home():
    saved = os.environ.get("OPTRA_HOME")
    with tempfile.TemporaryDirectory() as d:
        os.environ["OPTRA_HOME"] = d
        yield Path(d)
    os.environ.pop("OPTRA_HOME") if saved is None else os.environ.__setitem__("OPTRA_HOME", saved)


def test_runs_are_kept_found_rerun_and_compared(home):
    a = call_sync("price_option", **OPTION)
    b = call_sync("price_option", **{**OPTION, "spot": 101.0})
    listed = call_sync("list_runs", limit=5)["result_summary"]["runs"]
    assert [r["run_id"] for r in listed[:2]] == [b["run_id"], a["run_id"]] and listed[0]["headline"]["bs"] == b["result_summary"]["bs"]
    got = call_sync("get_run", run_id=a["run_id"], rerun=True)["result_summary"]
    assert got["result_summary"] == a["result_summary"] and got["rerun"]["differences"] == []  # seeded: it repeats
    diff = call_sync("compare_runs", run_a=a["run_id"], run_b=b["run_id"])["result_summary"]
    assert diff["inputs"] == [{"key": "spot", "a": 100.0, "b": 101.0, "change": 1.0, "relative_change": 0.01}]
    assert any(row["key"] == "bs" and row["change"] > 0 for row in diff["results"])
    with pytest.raises(ToolError) as e:
        call_sync("get_run", run_id="00000000-0000-0000-0000-000000000000")
    assert e.value.status == 404
    assert len(call_sync("list_runs", limit=50)["result_summary"]["runs"]) == 3  # the rerun is a run too; lookups are not


def test_history_over_http(home):
    with TestClient(APP) as c:
        run = c.post("/api/lab/pricing/run", json=OPTION).json()
        assert c.get(f"/api/runs/{run['run_id']}").json()["result_summary"]["tool"] == "price_option"
        assert c.delete(f"/api/runs/{run['run_id']}").json() == {"deleted": run["run_id"]}
        assert c.get(f"/api/runs/{run['run_id']}").status_code == 404


def test_old_runs_are_pruned(home):
    (home / "config.json").write_text(json.dumps({"runs": {"max_runs": 3, "max_days": 90}}))
    store = run_store.store()
    for i in range(6):
        store.save({"run_id": f"run-{i:04d}-0000", "tool": "t", "created_at": f"2026-10-10T00:00:0{i}+00:00",
                    "input_params": {}, "result_summary": {}})
        store._prune()
    assert [r["run_id"] for r in store.list(limit=10)] == ["run-0005-0000", "run-0004-0000", "run-0003-0000"]


# ── The Treasury curve ────────────────────────────────────────────────────────

def test_rate_curve_is_served_from_memory_and_disk(home, monkeypatch):
    calls = []

    async def download():
        calls.append(1)
        await asyncio.sleep(0.05)
        return {"1m": 0.041, "3m": 0.042, "6m": 0.043, "1y": 0.044, "2y": 0.045}, "2026-10-09"
    monkeypatch.setattr(market_data, "_download_rate_curve", download)
    monkeypatch.setattr(market_data, "_rate", market_data._KeptCurve())

    async def run():
        first = await market_data._fetch_rate_curve()          # nothing at all: waits for the site
        info = market_data.rate_curve_info()
        again = await market_data._fetch_rate_curve()          # fresh: from memory
        return first, info, again
    first, info, again = asyncio.run(run())
    assert first["3m"] == 0.042 and info["source"] == "treasury" and info["as_of"] == "2026-10-09" and len(calls) == 1
    assert again == first and len(calls) == 1
    assert json.loads((home / "rates.json").read_text())["curve"] == first
    # a new process: the saved curve at once; a day-old one is served while it refreshes in the background
    market_data._rate = market_data._KeptCurve()
    saved = json.loads((home / "rates.json").read_text())
    saved["fetched"] = time.time() - 2 * 3600
    (home / "rates.json").write_text(json.dumps(saved))

    async def stale():
        curve = await market_data._fetch_rate_curve()
        source = market_data.rate_curve_info()["source"]
        await asyncio.sleep(0.1)  # the background refresh
        return curve, source
    curve, source = asyncio.run(stale())
    assert curve == first and source == "saved" and len(calls) == 2


def test_rate_curve_falls_back_when_the_site_fails(home, monkeypatch):
    async def fail():
        raise OSError("down")
    monkeypatch.setattr(market_data, "_download_rate_curve", fail)
    monkeypatch.setattr(market_data, "_rate", market_data._KeptCurve())
    curve = asyncio.run(market_data._fetch_rate_curve())
    assert curve == market_data._FALLBACK_CURVE and market_data.rate_curve_info()["source"] == "fallback"
