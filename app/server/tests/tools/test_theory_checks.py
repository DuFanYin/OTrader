"""The theory checks measure something: sample moments come from a sample, and Itô's formula is checked on
discrete paths, so both errors shrink as the sample or the steps grow, and the default gate passes.

Run:  server/.venv/bin/python tests/test_theory_checks.py   (or pytest)
"""

from __future__ import annotations

import sys


from fastapi.testclient import TestClient  # noqa: E402

from main import create_app  # noqa: E402
from optra.infra.numerics import ito_check, stats_normal  # noqa: E402

APP = create_app(engine=False)  # the server without the live engine


def test_stats_are_sampled_and_converge():
    small, large = stats_normal(0.04, 0.36, 1.0, 100), stats_normal(0.04, 0.36, 1.0, 1_000_000)
    assert small["mean"] != small["mean_exact"]
    for k in ("mean", "variance", "mgf"):
        assert abs(large[k] - large[f"{k}_exact"]) < abs(small[k] - small[f"{k}_exact"])
        assert abs(large[k] - large[f"{k}_exact"]) < 3e-3


def test_ito_expectation_holds_for_every_function():
    for f, target in (("exp_martingale", 1.0), ("w2_minus_t", 0.0), ("w3", 0.0)):
        r = ito_check(f, 0.7, 1.0, 1000)
        assert r["target_expectation"] == target
        assert abs(r["value"] - target) <= 4 * r["std_error"] + 1e-12


def test_ito_formula_residual_shrinks_like_sqrt_dt():
    for f in ("exp_martingale", "w2_minus_t", "w3"):
        coarse, fine = ito_check(f, 0.7, 1.0, 100), ito_check(f, 0.7, 1.0, 10_000)
        ratio = fine["formula_residual"] / coarse["formula_residual"]
        assert 0.05 < ratio < 0.2, (f, ratio)  # √(100 / 10 000) = 0.1


def test_default_gate_passes_and_coarse_ito_fails_with_the_right_fix():
    body = {"spot": 100.0, "strike": 100.0, "rate": 0.05, "vol": 0.2, "maturity": 1.0, "mu": 0.05,
            "pick_stats": True, "pick_ito": True, "stats_n": 50_000, "ito_n": 5000}
    with TestClient(APP) as c:
        ok = c.post("/api/lab/validation/gate", json=body).json()
        coarse = c.post("/api/lab/validation/gate", json={**body, "ito_n": 100, "ito_function_type": "w2_minus_t"}).json()
    assert ok["result_summary"]["gate_decision"] == "go", ok["result_details"]["rows"]
    rows = {row["metric"]: row for row in coarse["result_details"]["rows"]}
    assert rows["formula_residual"]["status"] == "fail" and rows["expectation_gap"]["status"] == "pass"
    actions = [a["action_id"] for a in coarse["result_details"]["explainable_qa"]["minimal_fix_set"]]
    assert actions == ["increase_ito_steps"]


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
