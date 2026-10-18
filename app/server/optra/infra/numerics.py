"""The C++ numerics library as the server calls it: each task in one of POOL_PROCESSES processes that load the library
(python -m optra.infra.analytics_lib), so a crash or a hang in it is that task's error, not the server's end. A
process that crashed or hung is replaced; all are replaced when the library is built anew.

The domain helpers below are what the tools call; each raises EngineError when its task fails."""

from __future__ import annotations

import json
import os
import selectors
import subprocess
import sys
import threading
from pathlib import Path

import numpy as np

from optra.infra import analytics_lib
from optra.infra.analytics_lib import error

POOL_PROCESSES = 2
TASK_SECONDS = 600  # a task longer than this is taken as hung: its process is killed

_INITIAL_BUFFER = 1_000_000  # the answer's first buffer; the library asks for more when it needs it
_threads = 0  # threads a task runs on (set_num_threads); 0: the cores shared by the pool's processes


class _Gone(Exception):
    """The process died (a crash in the library) or was killed (a hang)."""


class _Process:
    def __init__(self, built: float | None) -> None:
        self.built = built
        self.proc = subprocess.Popen([sys.executable, "-m", "optra.infra.analytics_lib"],
                                     cwd=Path(__file__).resolve().parents[2], stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE)

    def ask(self, header: dict, request: bytes | None, seconds: float, matrix: bytes = b"") -> dict:
        try:
            self.proc.stdin.write(json.dumps(header, separators=(",", ":")).encode() + b"\n")
            if request is not None:
                self.proc.stdin.write(request + b"\n")
            self.proc.stdin.write(matrix)
            self.proc.stdin.flush()
        except BrokenPipeError as e:
            self.kill()
            raise _Gone("the numerics process had died") from e
        with selectors.DefaultSelector() as sel:
            sel.register(self.proc.stdout, selectors.EVENT_READ)
            if not sel.select(seconds):
                self.kill()
                raise _Gone(f"ran longer than {seconds:.0f} s: stopped")
        line = self.proc.stdout.readline()
        if not line:
            self.kill()
            raise _Gone(f"the numerics process crashed (exit {self.proc.returncode})")
        return json.loads(line)

    def kill(self) -> None:
        self.proc.kill()
        self.proc.wait()
        for pipe in (self.proc.stdin, self.proc.stdout):
            try:
                pipe.close()
            except BrokenPipeError:
                pass


_idle: list[_Process] = []
_slots = threading.BoundedSemaphore(POOL_PROCESSES)
_lock = threading.Lock()
_problem: tuple[float | None, str] | None = None  # (the library's time, why it cannot compute) once asked


def library_build() -> float | None:
    """The library's build (its file's time): a result it computed holds while this does."""
    where = analytics_lib.path()
    return where.stat().st_mtime if where else None


def _ask(header: dict, request: bytes | None = None, seconds: float = TASK_SECONDS, matrix: bytes = b"") -> dict:
    """A task (its header and its JSON request) to an idle process of the current build (started when there is
    none)."""
    built = library_build()
    with _slots:
        with _lock:
            while _idle and (_idle[-1].built != built or _idle[-1].proc.poll() is not None):
                _idle.pop().kill()  # an older build's, or one that died idle
            process = _idle.pop() if _idle else None
        process = process or _Process(built)
        answer = process.ask(header, request, seconds, matrix)  # _Gone: the process is dead, not given back
        with _lock:
            _idle.append(process)
        return answer


def shutdown() -> None:
    """The pool's processes stopped (the server's end)."""
    with _lock:
        while _idle:
            _idle.pop().kill()


def engine_problem() -> str:
    """Why the library cannot compute; empty when it can (asked once per build)."""
    global _problem
    built = library_build()
    if _problem is None or _problem[0] != built:
        try:
            _problem = (built, _ask({}, seconds=60)["problem"])
        except _Gone as e:
            return str(e)
    return _problem[1]


def is_engine_available() -> bool:
    return engine_problem() == ""


def set_num_threads(n_threads: int) -> int:
    """Threads each task runs on from now on."""
    global _threads
    _threads = max(1, int(n_threads))
    return _threads


def get_max_threads() -> int:
    return _threads or max(1, (os.cpu_count() or 1) // POOL_PROCESSES)


def run_engine_task(task_type: str, payload: dict, matrix: np.ndarray | None = None) -> dict:
    """One task of the library, in the pool: its answer {contract_version, status, result_summary, result_details,
    runtime_ms, error}; a crash or a hang comes back as the task's error. A matrix task (selection) takes its values
    beside the request, as float64 row by row, not in its JSON."""
    request = json.dumps(payload, separators=(",", ":")).encode()
    header = {"task": task_type, "threads": get_max_threads(), "buffer": _INITIAL_BUFFER}
    values = b""
    if matrix is not None:
        values = np.ascontiguousarray(matrix, dtype="<f8").tobytes()
        header["matrix"] = len(values) // 8
    try:
        return _ask(header, request, matrix=values)
    except _Gone as e:
        return error("engine_crashed", f"{task_type}: {e}")


def selection(returns: np.ndarray, tries: int, train: int = 0, test: int = 0) -> dict:
    """The selection statistics of trials' returns by period (trials x periods, NaN where a trial has none), with
    `tries` counted (engine/quant/stats/selection.cpp): {deflated_sharpe, pbo, walk_forward}."""
    trials, periods = returns.shape
    return checked_task("selection", {"trials": trials, "periods": periods, "tries": tries, "train": train,
                                      "test": test}, returns)["result_summary"]


class EngineError(RuntimeError):
    """A task the library refused or failed (status: the HTTP status that fits)."""

    def __init__(self, message: str, status: int = 400):
        super().__init__(message)
        self.status = status


def checked_task(task_type: str, payload: dict, matrix: np.ndarray | None = None) -> dict:
    """The task's answer when it succeeded; EngineError when not, never a made-up result."""
    r = run_engine_task(task_type, payload, matrix)
    if r["status"] != "ok":
        err = r["error"]
        raise EngineError(f"{task_type} failed: {err['message'] or err['code']}",
                          503 if err["code"] in ("engine_unavailable", "engine_crashed") else 400)
    return r


# ── Domain helpers ────────────────────────────────────────────────────────────
# Each indexes the fields its task always answers, so a contract mismatch fails here, loudly.

def pricing_bundle(
    spot: float, strike: float, rate: float, vol: float, maturity: float,
    n_paths: int, steps: int, dividend_yield: float, option_type: str = "call",
) -> dict:
    """The pricing task's result_summary: mc, bs, binomial, trinomial, greeks, ..."""
    return checked_task("pricing", {
        "spot": spot, "strike": strike, "rate": rate, "vol": vol,
        "maturity": maturity, "n_paths": n_paths, "dividend_yield": dividend_yield,
        "n_steps": int(steps), "option_type": option_type,
    })["result_summary"]


def stats_normal(mu: float, sigma: float, theta: float, sample_size: int) -> dict:
    s = checked_task("stats", {
        "mu": mu, "sigma": sigma, "theta": theta, "sample_size": sample_size,
    })["result_summary"]
    return {k: s[k] for k in ("mgf", "mean", "variance", "mgf_exact", "mean_exact", "variance_exact")}


def ito_check(function_type: str, theta: float, t: float, n_steps: int) -> dict:
    s = checked_task("ito_check", {
        "function_type": function_type, "theta": theta, "t": t, "n_steps": n_steps,
    })["result_summary"]
    return {k: s[k] for k in ("function_type", "value", "target_expectation", "std_error", "formula_residual", "paths")}


def measure_density_path(mu: float, r: float, sigma: float, t: float, n_steps: int) -> list[float]:
    return checked_task("measure_density", {
        "mu": mu, "r": r, "sigma": sigma, "t": t, "n_steps": n_steps,
    })["result_details"]["density"]


def pde_solve(
    spot: float, strike: float, rate: float, vol: float, maturity: float,
    dividend_yield: float, s_steps: int, t_steps: int, method: str,
    option_type: str = "call", is_american: bool = False,
) -> dict:
    """Finite-difference price plus grid diagnostics (t_steps_used, psor_iterations, ...)."""
    return checked_task("pde", {
        "spot": spot, "strike": strike, "rate": rate, "vol": vol,
        "maturity": maturity, "dividend_yield": dividend_yield,
        "s_steps": int(s_steps), "t_steps": int(t_steps), "method": method,
        "option_type": option_type, "is_american": is_american,
    })["result_summary"]


def lattice_prices(
    spot: float, strike: float, rate: float, vol: float, maturity: float, dividend_yield: float,
    steps: list[int], option_type: str = "call", is_american: bool = False,
) -> dict:
    """The lattices alone (no Monte Carlo): {"bs" (European), "steps", "binomial", "trinomial"}, a price per step
    count."""
    r = checked_task("lattice", {
        "spot": spot, "strike": strike, "rate": rate, "vol": vol, "maturity": maturity,
        "dividend_yield": dividend_yield, "steps": [int(n) for n in steps], "option_type": option_type,
        "is_american": is_american,
    })
    return {"bs": r["result_summary"]["bs"], **{k: r["result_details"][k] for k in ("steps", "binomial", "trinomial")}}


def measure_compare(
    mu: float, r: float, sigma: float, t: float,
    n_steps: int, n_paths: int, x0: float, preview_len: int = 50,
) -> dict:
    result = checked_task("measure_compare", {
        "mu": mu, "r": r, "sigma": sigma, "t": t,
        "n_steps": n_steps, "n_paths": n_paths, "x0": x0,
        "preview_len": preview_len,
    })
    s = result["result_summary"]
    return {"p_stats": s["p_stats"], "q_stats": s["q_stats"], "path_preview": result["result_details"]["path_preview"]}


def vol_surface_interp(
    surface: list[dict], spot: float, target_strike: float, target_expiry: float,
) -> tuple[float, str] | None:
    """(iv, method), or None when the surface is empty, gives no vol there or the engine cannot answer (the callers
    fall back to another vol)."""
    if not surface:
        return None
    r = run_engine_task("vol_surface", {
        "spot":           spot,
        "target_strike":  target_strike,
        "target_expiry":  target_expiry,
        "strikes":        [p["strike"] for p in surface],
        "expiries":       [p["years"]  for p in surface],
        "ivs":            [p["iv"]     for p in surface],
    })
    if r["status"] != "ok":
        return None
    s = r["result_summary"]
    if s["iv"] is None or s["iv"] <= 0:
        return None
    return float(s["iv"]), str(s["method"])


def greek_surface(
    strike: float, rate: float, vol: float, dividend_yield: float,
    spot_min: float, spot_max: float,
    mat_min: float = 0.1, mat_max: float = 3.0,
    n_spots: int = 21, n_mats: int = 11,
    greek: str = "delta",
) -> dict:
    r = checked_task("greek_surface", {
        "strike": strike, "rate": rate, "vol": vol, "dividend_yield": dividend_yield,
        "spot_min": spot_min, "spot_max": spot_max,
        "mat_min": mat_min, "mat_max": mat_max,
        "n_spots": n_spots, "n_mats": n_mats, "greek": greek,
    })
    s, d = r["result_summary"], r["result_details"]
    return {**{k: s[k] for k in ("greek", "grid_min", "grid_max", "n_spots", "n_mats")},
            **{k: d[k] for k in ("spots", "maturities", "grid")}}


def multi_leg(
    spot: float, rate: float, vol: float, maturity: float,
    legs: list[dict],
    dividend_yield: float = 0.0,
    n_paths: int = 10000,
) -> dict:
    """legs: list of {option_type, strike, quantity}"""
    r = checked_task("multi_leg", {
        "spot": spot, "rate": rate, "vol": vol, "maturity": maturity,
        "dividend_yield": dividend_yield, "n_paths": n_paths, "legs": legs,
    })
    s = r["result_summary"]
    return {**{k: s[k] for k in ("net_bs_price", "net_mc_price", "net_binomial_price", "net_delta", "net_vega",
                                 "strategy_hint", "n_legs")},
            "legs": r["result_details"]["legs"]}


def implied_vol_single(
    market_price: float, spot: float, strike: float,
    rate: float, maturity: float, dividend_yield: float = 0.0,
) -> dict:
    s = checked_task("implied_vol", {
        "market_price": market_price, "spot": spot, "strike": strike,
        "rate": rate, "maturity": maturity, "dividend_yield": dividend_yield,
    })["result_summary"]
    return {k: s[k] for k in ("implied_vol", "converged", "final_error")}


def implied_vol_batch(
    market_prices: list[float], strikes: list[float], expiries: list[float],
    spot: float, rate: float, dividend_yield: float = 0.0,
) -> dict:
    r = checked_task("implied_vol_batch", {
        "spot": spot, "rate": rate, "dividend_yield": dividend_yield,
        "market_prices": market_prices, "strikes": strikes, "expiries": expiries,
    })
    s, d = r["result_summary"], r["result_details"]
    return {"ivs": d["ivs"], "converged": d["converged"], "n_converged": s["n_converged"],
            "convergence_rate": s["convergence_rate"]}


def black_batch(is_call: list[bool], forwards: list[float], strikes: list[float], maturities: list[float],
                rate: float, *, vols: list[float] | None = None, prices: list[float] | None = None,
                spots: list[float] | None = None, greeks: bool = False) -> dict:
    """Black on each option's forward, discounted at rate: the trading engine's own pricing. With vols:
    {"prices"}, and with greeks also {"delta", "gamma", "theta", "vega"} (to spots, the forward when absent);
    with prices: {"ivs"} (0: none). Raises EngineError when the engine is missing or refuses."""
    payload = {"rate": rate, "is_call": is_call, "forwards": forwards, "strikes": strikes, "maturities": maturities}
    payload.update({"vols": vols, "greeks": greeks} if vols is not None else {"prices": prices})
    if spots is not None:
        payload["spots"] = spots
    return checked_task("black_batch", payload)["result_details"]


def heston_price(
    spot: float, strike: float, rate: float, maturity: float,
    v0: float = 0.04, kappa: float = 1.5, theta: float = 0.04,
    xi: float = 0.5, rho: float = -0.7,
) -> float:
    return checked_task("heston_price", {
        "spot": spot, "strike": strike, "rate": rate, "maturity": maturity,
        "v0": v0, "kappa": kappa, "theta": theta, "xi": xi, "rho": rho,
    })["result_summary"]["heston_price"]


def heston_calibrate(
    spot: float, rate: float,
    market_strikes: list[float], market_maturities: list[float], market_prices: list[float],
    dividend_yield: float = 0.0,
    init_v0: float = 0.04, init_kappa: float = 1.5, init_theta: float = 0.04,
    init_xi: float = 0.5, init_rho: float = -0.7,
    max_iter: int = 500,
) -> dict:
    r = checked_task("heston_calibrate", {
        "spot": spot, "rate": rate, "dividend_yield": dividend_yield,
        "market_strikes": market_strikes, "market_maturities": market_maturities,
        "market_prices": market_prices,
        "init_v0": init_v0, "init_kappa": init_kappa, "init_theta": init_theta,
        "init_xi": init_xi, "init_rho": init_rho, "max_iter": max_iter,
    })
    s, d = r["result_summary"], r["result_details"]
    return {**{k: s[k] for k in ("v0", "kappa", "theta", "xi", "rho", "rmse", "max_abs_error", "iterations",
                                 "converged")},
            "model_prices": d["model_prices"], "residuals": d["residuals"]}


def screener(request: dict) -> dict:
    """Run the C++ strategy screener.

    `request` follows the screener task's contract (chain + filters + rank).
    Returns the raw engine envelope so callers can surface engine error codes
    (e.g. bad_range_rr_range) instead of an empty result.
    """
    return run_engine_task("screener", request)


def run_convergence_steps(
    spot: float, strike: float, rate: float, vol: float,
    maturity: float, dividend_yield: float,
    step_ladder: list[int],
    option_type: str = "call",
) -> list[dict]:
    """Binomial and trinomial lattice prices on a step ladder against the BS reference."""
    lat = lattice_prices(spot, strike, rate, vol, maturity, dividend_yield, [max(2, int(n)) for n in step_ladder],
                         option_type)
    bs_ref = lat["bs"]
    rows = []
    prev_err = prev_tri = None
    for st, binomial, trinomial in zip(lat["steps"], lat["binomial"], lat["trinomial"], strict=True):
        abs_err, tri_err = abs(binomial - bs_ref), abs(trinomial - bs_ref)
        rows.append({
            "steps": st, "bs_ref": bs_ref, "binomial": binomial,
            "abs_error": abs_err, "rel_error": abs_err / max(abs(bs_ref), 1e-10),
            "error_ratio_vs_prev": (abs_err / prev_err) if prev_err else None,
            "trinomial": trinomial, "trinomial_abs_error": tri_err,
            "trinomial_error_ratio_vs_prev": (tri_err / prev_tri) if prev_tri else None,
        })
        prev_err, prev_tri = abs_err, tri_err
    return rows
