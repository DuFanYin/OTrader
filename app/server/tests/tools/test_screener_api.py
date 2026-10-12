"""Screener API: /tool/screener/*, multi-leg per-leg overrides, portfolio stress.

Offline: Deribit is served from the recorded fixture (see test_market_chain.FakeDeribit).
Run:  server/.venv/bin/python tests/test_screener_api.py   (or pytest)
"""

from __future__ import annotations

import math
import sys

from fastapi.testclient import TestClient  # noqa: E402

from main import create_app  # noqa: E402
from test_market_chain import Env, FakeDeribit  # noqa: E402

APP = create_app(engine=False)  # the server without the live engine


def _black76(F, K, r, vol, T, is_call):
    n = lambda x: 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))  # noqa: E731
    sd = vol * math.sqrt(T)
    d1 = (math.log(F / K) + 0.5 * sd * sd) / sd
    d2 = d1 - sd
    df = math.exp(-r * T)
    return df * (F * n(d1) - K * n(d2)) if is_call else df * (K * n(-d2) - F * n(-d1))


BASE = {
    "currency": "BTC",
    "strategies": {"iron_condors": True, "strangles": True},
    "option_filter": {"days_to_expiry_range": [1, 45], "max_bid_ask_spread_pct": 0.25},
    "rank": {"key": "rr", "top_n": 10},
}


def client():
    return TestClient(APP)


# ── Screener routes ───────────────────────────────────────────────────────────

def test_run_on_live_chain():
    with Env(FakeDeribit()), client() as c:
        r = c.post("/api/lab/screener/run", json=BASE)
        assert r.status_code == 200, r.text
        d = r.json()
    s = d["result_summary"]
    assert s["source"] == "live" and s["currency"] == "BTC" and s["rate"] == 0.0 and s["multiplier"] == 1.0
    assert s["n_generated"] > 0 and s["n_returned"] == len(d["result_details"]["strategies"]) == 10
    assert d["warnings"] == [] and d["result_summary"]["chain_quality"]["options"] > 0
    strat = d["result_details"]["strategies"][0]
    assert strat["legs"] and all({"forward", "years", "iv", "fill_price"} <= set(l) for l in strat["legs"])
    assert d["result_details"]["expiries"]


def test_offline_falls_back_to_fixture_with_note():
    with Env(FakeDeribit(fail=True)), client() as c:
        d = c.post("/api/lab/screener/run", json=BASE).json()
    assert d["result_summary"]["source"] == "fixture"
    assert any("fixture" in w for w in d["warnings"])


def test_snapshots_and_chain():
    with Env(FakeDeribit()), client() as c:
        c.post("/api/lab/screener/run", json=BASE)                       # writes a snapshot
        snaps = c.get("/api/lab/screener/snapshots", params={"currency": "BTC"}).json()["result_summary"]["snapshots"]
        assert len(snaps) == 1
        sid = snaps[0]["snapshot_id"]

        d = c.post("/api/lab/screener/run", json={**BASE, "snapshot_id": sid}).json()
        assert d["result_summary"]["source"] == "cache"

        assert c.post("/api/lab/screener/run", json={**BASE, "currency": "ETH", "snapshot_id": sid}).status_code == 400
        assert c.post("/api/lab/screener/run", json={**BASE, "snapshot_id": "BTC_19990101_000000"}).status_code == 404
        assert c.get("/api/lab/screener/snapshots", params={"currency": "DOGE"}).status_code == 422

        full = c.get("/api/lab/screener/chain", params={"currency": "btc"}).json()
        short = c.get("/api/lab/screener/chain", params={"currency": "BTC", "max_days": 7}).json()
    assert full["result_summary"]["n_options"] > short["result_summary"]["n_options"] > 0
    assert all(r["years"] * 365 <= 7 for r in short["result_details"]["chain"])
    assert all(e["years"] * 365 <= 7 for e in short["result_details"]["expiries"])


def test_model_vol_modes():
    with Env(FakeDeribit()), client() as c:
        # DVOL endpoint is not served by the fake -> fallback value, flagged in warnings
        d = c.post("/api/lab/screener/run", json={**BASE, "model_vol": "dvol"}).json()
        assert d["result_summary"]["model_vol"] == "flat" and d["result_summary"]["model_vol_requested"] == "dvol"
        assert any("fallback" in w for w in d["warnings"])

        d = c.post("/api/lab/screener/run", json={**BASE, "model_vol": "surface"}).json()
        assert all(s["model_value"] is not None for s in d["result_details"]["strategies"])

        d = c.post("/api/lab/screener/run", json={**BASE, "model_vol": "flat", "model_vol_flat": 0.5}).json()
        assert d["result_summary"]["model_vol"] == "flat"

        heston = {"v0": 0.16, "kappa": 2.0, "theta": 0.16, "xi": 0.5, "rho": -0.3}
        d = c.post("/api/lab/screener/run", json={**BASE, "model_vol": "heston", "heston": heston}).json()
        assert d["result_summary"]["model_vol"] == "heston"

        # no parameters: Heston is calibrated to the chain first
        d = c.post("/api/lab/screener/run", json={**BASE, "model_vol": "heston"}).json()
        fit = d["result_summary"]["heston_calibrated"]
        assert d["result_summary"]["model_vol"] == "heston" and fit["quotes"] >= 6 and fit["rmse_rel"] < 0.05
        assert -1 <= fit["rho"] <= 1 and fit["v0"] > 0 and any("Heston calibrated" in n for n in d["diagnostics"]["notes"])
        assert c.post("/api/lab/screener/run", json={**BASE, "model_vol": "flat"}).status_code == 422


def test_request_validation():
    with Env(FakeDeribit()), client() as c:
        assert c.post("/api/lab/screener/run", json={**BASE, "strategies": {}}).status_code == 400
        assert c.post("/api/lab/screener/run", json={**BASE, "currency": "SOL"}).status_code == 422
        bad_range = {**BASE, "strategy_filter": {"rr_range": [1]}}
        assert c.post("/api/lab/screener/run", json=bad_range).status_code == 422
        open_range = {**BASE, "strategy_filter": {"rr_range": [0.05, None]}}
        d = c.post("/api/lab/screener/run", json=open_range).json()
        assert all(s["rr"] is None or s["rr"] >= 0.05 for s in d["result_details"]["strategies"])


# ── Multi-leg per-leg overrides ──────────────────────────────────────────────

def test_multi_leg_forward_leg_is_black76():
    F, K, vol, T = 84000.0, 90000.0, 0.55, 0.2
    with client() as c:
        d = c.post("/api/lab/pricing/multi-leg", json={
            "spot": 80000, "rate": 0.0, "vol": 0.9, "maturity": 1.0,
            "legs": [{"option_type": "call", "strike": K, "quantity": 1, "vol": vol, "maturity": T, "forward": F}],
        }).json()
    leg = d["result_details"]["legs"][0]
    assert abs(leg["bs_price"] - _black76(F, K, 0.0, vol, T, True)) < 1e-4
    assert leg["vol"] == vol and leg["maturity"] == T and leg["forward"] == F


def test_multi_leg_without_overrides_unchanged():
    legs = [{"option_type": "call", "strike": 100, "quantity": 1}, {"option_type": "put", "strike": 100, "quantity": 1}]
    with client() as c:
        a = c.post("/api/lab/pricing/multi-leg", json={"spot": 100, "rate": 0.03, "vol": 0.2, "maturity": 1, "legs": legs}).json()
        b = c.post("/api/lab/pricing/multi-leg", json={"spot": 100, "rate": 0.03, "vol": 0.2, "maturity": 1, "legs": [
            {**l, "vol": 0.2, "maturity": 1} for l in legs]}).json()
    assert a["result_summary"]["strategy_hint"] == "straddle"
    assert abs(a["result_summary"]["net_bs_price"] - b["result_summary"]["net_bs_price"]) < 1e-12


def test_multi_leg_hints_for_screener_shapes():
    ic = [{"option_type": "call", "strike": 110, "quantity": -1}, {"option_type": "call", "strike": 120, "quantity": 1},
          {"option_type": "put", "strike": 90, "quantity": -1}, {"option_type": "put", "strike": 80, "quantity": 1}]
    cal = [{"option_type": "call", "strike": 100, "quantity": -1, "maturity": 0.1},
           {"option_type": "call", "strike": 100, "quantity": 1, "maturity": 0.5}]
    base = {"spot": 100, "rate": 0.0, "vol": 0.3, "maturity": 0.25}
    with client() as c:
        hint = lambda legs: c.post("/api/lab/pricing/multi-leg", json={**base, "legs": legs}).json()["result_summary"]["strategy_hint"]  # noqa: E731
        assert hint(ic) == "iron_condor"
        assert hint([{**l, "quantity": -l["quantity"]} for l in ic]) == "reverse_iron_condor"
        assert hint(cal) == "calendar"


def test_screener_strategy_hands_off_to_multi_leg():
    """The legs the frontend builds from a screener row price back to the screener's own model value."""
    with Env(FakeDeribit()), client() as c:
        d = c.post("/api/lab/screener/run", json={**BASE, "strategies": {"iron_condors": True}}).json()
        s, summary = d["result_details"]["strategies"][0], d["result_summary"]
        legs = [{"option_type": l["option_type"], "strike": l["strike"], "quantity": l["qty"],
                 "vol": l["iv"], "maturity": l["years"], "forward": l["forward"]} for l in s["legs"]]
        ml = c.post("/api/lab/pricing/multi-leg", json={
            "spot": summary["spot"], "rate": summary["rate"], "vol": 0.5, "maturity": 0.1, "legs": legs,
        }).json()["result_summary"]
    assert ml["strategy_hint"] == "iron_condor"
    assert abs(ml["net_bs_price"] - s["model_value"]) < 1e-3 * max(1.0, abs(s["model_value"]))
    assert abs(ml["net_delta"] - s["net_delta"]) < 1e-3


# ── Portfolio stress ──────────────────────────────────────────────────────────

def test_portfolio_stress():
    legs = [{"option_type": "call", "strike": 90000, "quantity": 1, "vol": 0.5, "maturity": 0.1, "forward": 84000},
            {"option_type": "put", "strike": 78000, "quantity": 1, "vol": 0.55, "maturity": 0.1, "forward": 84000}]
    req = {"spot": 83500, "strike": 83500, "rate": 0.0, "vol": 0.5, "maturity": 0.1, "n_paths": 20000,
           "stress_pack": "core4", "legs": legs}
    with client() as c:
        d = c.post("/api/lab/stress/run", json=req).json()
        single = c.post("/api/lab/stress/run", json={**req, "legs": None, "include_hedge_compare": False}).json()
    s = d["result_summary"]
    assert s["mode"] == "portfolio" and s["n_legs"] == 2 and s["hedge_compare"] is True
    rows = {r["scenario"]: r for r in d["result_details"]["rows"]}
    # the position is priced on a tree too (250 steps a year: within a few percent here), and hedged in every scenario
    assert all(abs(r["binomial"] - r["bs"]) < 0.03 * r["bs"] and r["hedge"]["es95"] is not None for r in rows.values())
    assert rows["Gap Move"]["bs_shift_vs_base"] > 0          # a long strangle gains on a gap
    assert single["result_summary"]["mode"] == "single_option"
    assert all(r["binomial"] is not None for r in single["result_details"]["rows"])


if __name__ == "__main__":
    failed = 0
    for name, fn in sorted((n, f) for n, f in globals().items() if n.startswith("test_") and callable(f)):
        try:
            fn()
            print(f"ok    {name}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL  {name}: {e}")
    sys.exit(1 if failed else 0)
