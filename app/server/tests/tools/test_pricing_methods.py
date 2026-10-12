"""Pricing methods added from QF-205 and Option-Pricing: trinomial, LSM, explicit / American PDE, put and dividend handling.

Run:  server/.venv/bin/python tests/test_pricing_methods.py   (or pytest)
"""

from __future__ import annotations

import math
import sys


from fastapi.testclient import TestClient  # noqa: E402

from main import create_app  # noqa: E402
from optra.infra import numerics  # noqa: E402
from optra.infra.numerics import run_engine_task  # noqa: E402

APP = create_app(engine=False)  # the server without the live engine

BASE = {"spot": 100.0, "strike": 100.0, "rate": 0.05, "vol": 0.2, "maturity": 1.0}


def N(x):
    return 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))


def bs(s, k, r, q, v, t, call=True):
    d1 = (math.log(s / k) + (r - q + 0.5 * v * v) * t) / (v * math.sqrt(t))
    d2 = d1 - v * math.sqrt(t)
    if call:
        return s * math.exp(-q * t) * N(d1) - k * math.exp(-r * t) * N(d2)
    return k * math.exp(-r * t) * N(-d2) - s * math.exp(-q * t) * N(-d1)


def pricing(**kw):
    r = run_engine_task("pricing", {**BASE, "n_paths": 20000, **kw})
    assert r["status"] == "ok", r.get("error")
    return r["result_summary"]


def pde(**kw):
    r = run_engine_task("pde", {**BASE, "s_steps": 200, "t_steps": 200, **kw})
    assert r["status"] == "ok", r.get("error")
    return r["result_summary"]


# ── Put and dividend handling (fixed in the port) ────────────────────────────

def test_put_is_priced_as_put_through_the_api():
    with TestClient(APP) as c:
        put = c.post("/api/lab/pricing/run", json={**BASE, "option_type": "put", "n_paths": 20000}).json()["result_summary"]
        call = c.post("/api/lab/pricing/run", json={**BASE, "option_type": "call", "n_paths": 20000}).json()["result_summary"]
    assert put["option_type"] == "put"
    assert abs(put["bs"] - bs(100, 100, 0.05, 0, 0.2, 1, call=False)) < 1e-9
    # put-call parity on the closed form
    assert abs(call["bs"] - put["bs"] - (100 - 100 * math.exp(-0.05))) < 1e-9


def test_dividend_yield_closed_form_and_greeks():
    q, t = 0.03, 1.0
    for call in (True, False):
        s = pricing(dividend_yield=q, option_type="call" if call else "put", n_steps=2000)
        exact = bs(100, 100, 0.05, q, 0.2, t, call)
        assert abs(s["bs"] - exact) < 1e-9, (call, s["bs"], exact)
        assert abs(s["binomial"] - exact) < 5e-3 and abs(s["trinomial"] - exact) < 5e-3
        assert abs(s["mc"] - exact) < 4 * s["mc_std_err"]
        h = 1e-4
        fd_delta = (bs(100 + h, 100, 0.05, q, 0.2, t, call) - bs(100 - h, 100, 0.05, q, 0.2, t, call)) / (2 * h)
        fd_rho = (bs(100, 100, 0.05 + h, q, 0.2, t, call) - bs(100, 100, 0.05 - h, q, 0.2, t, call)) / (2 * h)
        fd_theta = -(bs(100, 100, 0.05, q, 0.2, t + h, call) - bs(100, 100, 0.05, q, 0.2, t - h, call)) / (2 * h)
        g = s["greeks"]
        assert abs(g["delta_bs"] - fd_delta) < 1e-6, (call, g["delta_bs"], fd_delta)
        assert abs(g["rho_bs"] - fd_rho) < 1e-4, (call, g["rho_bs"], fd_rho)
        assert abs(g["theta_bs"] - fd_theta) < 1e-4, (call, g["theta_bs"], fd_theta)


# ── Trinomial ─────────────────────────────────────────────────────────────────

def test_trinomial_converges_to_bs_with_order_one():
    rows = numerics.run_convergence_steps(100, 100, 0.05, 0.2, 1.0, 0.0, [25, 50, 100, 200, 400, 800])
    errs = [r["trinomial_abs_error"] for r in rows]
    slope = math.log(errs[-1] / errs[0]) / math.log(800 / 25)
    assert -1.3 < slope < -0.7, slope
    assert errs[-1] < 3e-3, errs


# ── American: lattices, PDE, LSM ─────────────────────────────────────────────

def test_american_methods_agree():
    s = pricing(option_type="put", is_american=True, n_steps=2000, lsm_paths=100000, lsm_steps=100)
    am = s["american_methods"]
    ref = am["binomial"]
    assert ref > s["bs"]                                       # early exercise premium on a put
    assert abs(am["trinomial"] / ref - 1) < 5e-4
    assert abs(am["pde_psor"] / ref - 1) < 5e-3                # acceptance: PDE vs binomial < 0.5%
    assert abs(am["lsm"] / ref - 1) < 1e-2                     # acceptance: LSM vs binomial < 1%


def test_american_call_without_dividends_has_no_premium():
    s = pricing(option_type="call", is_american=True, n_steps=1000)
    am = s["american_methods"]
    assert abs(am["early_exercise_premium"]) < 1e-9
    assert abs(am["trinomial"] - s["trinomial"]) < 1e-9
    with_q = pricing(option_type="call", is_american=True, n_steps=1000, dividend_yield=0.08)
    assert with_q["american_methods"]["early_exercise_premium"] > 1e-3   # dividends make it worth exercising


def test_lsm_is_deterministic_across_thread_counts():
    prev = numerics.get_max_threads()
    try:
        numerics.set_num_threads(1)
        a = pricing(option_type="put", is_american=True, lsm_paths=30000)["american_methods"]
        numerics.set_num_threads(8)
        b = pricing(option_type="put", is_american=True, lsm_paths=30000)["american_methods"]
    finally:
        numerics.set_num_threads(prev)
    assert a["lsm"] == b["lsm"] and a["lsm_std_err"] == b["lsm_std_err"]


# ── PDE schemes ───────────────────────────────────────────────────────────────

def test_explicit_matches_crank_nicolson_when_stable():
    for ot in ("call", "put"):
        # dt = 1/2000 is inside the explicit stability limit for 200 S-steps (needs >= 1585)
        cn = pde(method="crank_nicolson", option_type=ot, s_steps=200, t_steps=2000)
        ex = pde(method="explicit", option_type=ot, s_steps=200, t_steps=2000)
        assert not ex["stability_refined"]
        assert abs(ex["price"] - cn["price"]) < 1e-3, (ot, ex["price"], cn["price"])   # acceptance
        assert abs(cn["price"] - bs(100, 100, 0.05, 0, 0.2, 1, ot == "call")) < 2e-2


def test_explicit_refines_unstable_grid():
    out = pde(method="explicit", s_steps=200, t_steps=50)
    need = 1.0 * (0.04 * 199 ** 2 + 0.05)
    assert out["stability_refined"] and out["t_steps_used"] >= need
    assert abs(out["price"] - bs(100, 100, 0.05, 0, 0.2, 1)) < 1e-2


def test_pde_route_reports_reference_and_grid():
    with TestClient(APP) as c:
        r = c.post("/api/lab/pde/run", json={**BASE, "method": "implicit", "option_type": "put",
                                              "is_american": True, "s_steps": 200, "t_steps": 200}).json()
        s = r["result_summary"]
        assert s["reference_method"] == "binomial_american" and s["psor_iterations"] > 0
        assert abs(s["price_vs_bs_gap"]) / s["reference_price"] < 5e-3
        bad = c.post("/api/lab/pde/run", json={**BASE, "method": "adi"})
        assert bad.status_code == 422


def test_benchmark_and_convergence_include_trinomial():
    with TestClient(APP) as c:
        bm = c.post("/api/lab/benchmark/run", json={"pricing": {**BASE, "option_type": "put"},
                                                     "benchmark_baseline": "trinomial"}).json()
        methods = {r["method"] for r in bm["result_details"]["rows"]}
        assert methods == {"mc", "bs", "binomial", "trinomial", "pde"}
        assert bm["result_summary"]["baseline_method"] == "trinomial"
        cv = c.post("/api/lab/convergence/run", json={**BASE, "option_type": "put"}).json()
        assert cv["result_summary"]["option_type"] == "put"
        assert -1.3 < cv["result_summary"]["trinomial"]["log_slope"] < -0.7
        assert len(cv["result_details"]["trinomial_curve_points"]) == cv["result_summary"]["ladder_size"]


def test_validation_gate_lattice_checks():
    with TestClient(APP) as c:
        r = c.post("/api/lab/validation/gate", json={**BASE, "pick_lattice": True}).json()
    rows = {row["metric"]: row for row in r["result_details"]["rows"]}
    assert set(rows) == {"trinomial_vs_bs_rel", "lsm_vs_binomial_american_rel", "pde_vs_binomial_american_rel"}
    assert all(row["status"] == "pass" for row in rows.values())
    assert r["result_summary"]["gate_decision"] == "go"


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
