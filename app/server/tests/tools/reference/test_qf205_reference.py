"""C++ lattices / finite differences vs the QF-205 Python implementations (numerical reference).

QF-205 is used as an external package, not copied. Run with an interpreter that has
numpy, e.g. QF-205's own venv (the engine client only needs the standard library):

    ../QF-205/.venv/bin/python tests/reference/test_qf205_reference.py

QF205_SRC overrides the location of QF-205/src. Skipped when it cannot be imported.

Same algorithm, same grid -> results should agree to float noise. The port fixes
three QF-205 finite-difference details (boundary tau, American boundary floor,
PSOR double-counted boundary term); on these grids their effect stays far below
the tolerance, which this test also demonstrates.
"""

from __future__ import annotations

import itertools
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "server"))
QF205_SRC = Path(os.environ.get("QF205_SRC", ROOT.parent / "QF-205" / "src"))
sys.path.insert(0, str(QF205_SRC))

from optra.infra.numerics import run_engine_task  # noqa: E402

try:
    from option_calculator.pricing import (  # noqa: E402
        FDGrid, price_binomial_crr, price_fd_crank_nicolson, price_fd_explicit,
        price_fd_implicit, price_trinomial_boyle,
    )
    from option_calculator.types import ContractParams, ExerciseStyle, MarketParams, OptionType  # noqa: E402
except ImportError as exc:  # numpy or QF-205 missing
    if __name__ != "__main__":  # collected by pytest: skip this module, not the whole run
        import pytest
        pytest.skip(f"QF-205 reference unavailable ({exc})", allow_module_level=True)
    print(f"SKIP: QF-205 reference unavailable ({exc})")
    sys.exit(0)

CASES = [
    # spot, strike, rate, q, vol, T
    (100.0, 100.0, 0.05, 0.00, 0.20, 1.00),
    (100.0, 110.0, 0.03, 0.02, 0.35, 0.50),
    (80.0, 100.0, 0.08, 0.00, 0.25, 2.00),
    (120.0, 100.0, 0.01, 0.04, 0.15, 0.25),
]
TYPES = ["call", "put"]
EXERCISE = [False, True]
LATTICE_STEPS = 200
FD_M, FD_N = 100, 1300           # explicit-stable for every case (no time-grid refinement)


def rel(a: float, b: float) -> float:
    return abs(a - b) / max(abs(b), 1e-12)


def qf_inputs(case, option_type, american):
    s, k, r, q, v, t = case
    m = MarketParams(s0=s, r=r, q=q, sigma=v)
    c = ContractParams(k=k, t=t, option_type=OptionType(option_type),
                       exercise=ExerciseStyle.american if american else ExerciseStyle.european)
    return m, c


def engine_lattices(case, option_type, american):
    s, k, r, q, v, t = case
    out = run_engine_task("pricing", {
        "spot": s, "strike": k, "rate": r, "vol": v, "maturity": t, "dividend_yield": q,
        "n_paths": 100, "n_steps": LATTICE_STEPS, "option_type": option_type,
        "is_american": american, "lsm_paths": 1000, "lsm_steps": 5,
    })["result_summary"]
    if american:
        am = out["american_methods"]
        return am["binomial"], am["trinomial"]
    return out["binomial"], out["trinomial"]


def engine_pde(case, option_type, american, method):
    s, k, r, q, v, t = case
    out = run_engine_task("pde", {
        "spot": s, "strike": k, "rate": r, "vol": v, "maturity": t, "dividend_yield": q,
        "s_steps": FD_M, "t_steps": FD_N, "method": method,
        "option_type": option_type, "is_american": american,
    })["result_summary"]
    assert not out["stability_refined"], (case, method)
    return out["price"]


def test_lattices_match_qf205():
    worst = 0.0
    for case, ot, am in itertools.product(CASES, TYPES, EXERCISE):
        m, c = qf_inputs(case, ot, am)
        bin_cpp, tri_cpp = engine_lattices(case, ot, am)
        for name, cpp, py in [("binomial", bin_cpp, price_binomial_crr(m, c, LATTICE_STEPS)),
                              ("trinomial", tri_cpp, price_trinomial_boyle(m, c, LATTICE_STEPS))]:
            e = rel(cpp, py)
            worst = max(worst, e)
            assert e < 1e-9, (name, case, ot, am, cpp, py)
    print(f"      lattices: worst relative difference {worst:.2e}")


def _corrected_boundaries(fd_module, grid_n):
    """QF-205's boundary function with the two fixes the C++ port makes.

    QF-205 evaluates step n's boundary at t = n*dt, i.e. tau = T - n*dt: the time-0
    discount on the first backward step. The value being solved sits at
    tau = (n+1)*dt. American boundaries are also floored at intrinsic value.
    """
    original = fd_module._fd_boundary_values

    def patched(s, t, market, contract):
        dt = contract.t / grid_n
        tau = t + dt
        v0, vmax = original(s, contract.t - tau, market, contract)
        if contract.exercise == ExerciseStyle.american:
            k = contract.k
            smax = float(s[-1])
            if contract.option_type == OptionType.put:
                v0 = max(v0, k)
            else:
                vmax = max(vmax, smax - k)
        return v0, vmax

    return original, patched


def test_finite_differences_match_qf205():
    from option_calculator.methods import fd as fd_module

    grid = FDGrid(m=FD_M, n=FD_N)
    py_methods = {"crank_nicolson": price_fd_crank_nicolson, "implicit": price_fd_implicit,
                  "explicit": price_fd_explicit}
    original, patched = _corrected_boundaries(fd_module, FD_N)
    worst_fixed = worst_raw = worst_psor = 0.0
    for case, ot, am in itertools.product(CASES, TYPES, EXERCISE):
        m, c = qf_inputs(case, ot, am)
        for method, fn in py_methods.items():
            cpp = engine_pde(case, ot, am, method)
            raw = fn(m, c, grid)
            fd_module._fd_boundary_values = patched
            try:
                fixed = fn(m, c, grid)
            finally:
                fd_module._fd_boundary_values = original
            worst_raw = max(worst_raw, rel(cpp, raw))
            # And the corrections themselves stay small on these grids.
            assert rel(cpp, raw) < 1e-3, (method, case, ot, am, cpp, raw)
            if am and method != "explicit":
                # PSOR: QF-205 also counts the boundary term twice at the edge nodes
                # (inside its iteration loop, so it cannot be patched here); see
                # test_american_call_without_dividends_equals_european.
                worst_psor = max(worst_psor, rel(cpp, fixed))
                assert rel(cpp, fixed) < 1e-3, (method, case, ot, am, cpp, fixed)
            else:
                # Same algorithm once QF-205's boundaries are corrected.
                worst_fixed = max(worst_fixed, rel(cpp, fixed))
                assert rel(cpp, fixed) < 1e-6, (method, case, ot, am, cpp, fixed)
    print(f"      finite differences: worst relative difference {worst_fixed:.2e} "
          f"(American PSOR {worst_psor:.2e}; vs unmodified QF-205 {worst_raw:.2e})")


def test_american_call_without_dividends_equals_european():
    """No dividends: early exercise of a call is never optimal, so American == European.

    The C++ PSOR solver satisfies this; QF-205's does not, because its PSOR loop adds
    the S_max boundary term a second time (already moved into the right-hand side).
    """
    case = (80.0, 100.0, 0.08, 0.0, 0.25, 2.0)
    m, c_eu = qf_inputs(case, "call", False)
    _, c_am = qf_inputs(case, "call", True)
    grid = FDGrid(m=FD_M, n=FD_N)
    for method, fn in {"crank_nicolson": price_fd_crank_nicolson, "implicit": price_fd_implicit}.items():
        eu, am = engine_pde(case, "call", False, method), engine_pde(case, "call", True, method)
        assert rel(am, eu) < 1e-7, (method, am, eu)
        qf_gap = rel(fn(m, c_am, grid), fn(m, c_eu, grid))
        print(f"      {method}: C++ American/European gap {rel(am, eu):.1e}, QF-205 gap {qf_gap:.1e}")


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
