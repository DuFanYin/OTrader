import json
import os
from ctypes import CDLL, POINTER, byref, c_char, c_char_p, c_int, create_string_buffer
from pathlib import Path

from optra.version import CONTRACT_VERSION

# ── Library loading ──────────────────────────────────────────────────────────

ENGINE_PROBLEM = ""  # why the engine is not available, when it is not


def _engine_paths() -> list[Path]:
    """Where the engine may be: $OPTRA_ANALYTICS_LIB, else the repository's engine build (engine/build/quant/analytics)."""
    repo = Path(__file__).resolve().parents[4]  # the repository (this file is app/server/optra/infra/)
    paths = [repo / "engine" / "build" / "quant" / "analytics" / "libanalytics_c.so"]
    if os.environ.get("OPTRA_ANALYTICS_LIB"):
        paths.insert(0, Path(os.environ["OPTRA_ANALYTICS_LIB"]))
    return paths


def _load_lib() -> CDLL | None:
    global ENGINE_PROBLEM
    path = next((p for p in _engine_paths() if p.exists()), None)
    if path is None:
        ENGINE_PROBLEM = "the C++ engine is not built: run ./build.sh r at the repository root"
        return None
    lib = CDLL(str(path))
    version = None
    if hasattr(lib, "quant_contract_version"):
        lib.quant_contract_version.restype = c_char_p
        version = lib.quant_contract_version().decode()
    if version != CONTRACT_VERSION:
        ENGINE_PROBLEM = (f"the C++ engine at {path} speaks contract {version or 'v1'}, the server {CONTRACT_VERSION}: "
                          "rebuild it (./build.sh r at the repository root)")
        return None
    return lib


_lib = _load_lib()

# Initial response buffer; run_engine_task grows it once if the engine asks for more.
_INITIAL_BUFFER = 1_000_000

if _lib is not None:
    _TASK_TO_SYMBOL = {
        "pricing": "quant_run_pricing_json",
        "pricing_batch": "quant_run_pricing_batch_json",
        "pricing_batch_grid": "quant_run_pricing_batch_grid_json",
        "hedging": "quant_run_hedging_json",
        "scenario": "quant_run_scenario_json",
        "validation_gate": "quant_run_validation_json",
        "simulation": "quant_run_simulation_json",
        "stats": "quant_run_stats_json",
        "ito_check": "quant_run_ito_json",
        "measure_density": "quant_run_measure_density_json",
        "measure_compare": "quant_run_measure_compare_json",
        "pde": "quant_run_pde_json",
        "vol_surface": "quant_run_vol_surface_json",
        "greek_surface": "quant_run_greek_surface_json",
        "multi_leg":           "quant_run_multi_leg_json",
        "implied_vol":         "quant_run_implied_vol_json",
        "implied_vol_batch":   "quant_run_implied_vol_batch_json",
        "black_batch":         "quant_run_black_batch_json",
        "heston_calibrate":    "quant_run_heston_calibrate_json",
        "heston_price":        "quant_run_heston_price_json",
        "screener":            "quant_run_screener_json",
    }
    for _sym in _TASK_TO_SYMBOL.values():
        fn = getattr(_lib, _sym)
        fn.argtypes = [c_char_p, POINTER(c_char), c_int, POINTER(c_int)]
        fn.restype = c_int
    _lib.quant_set_num_threads.argtypes = [c_int]
    _lib.quant_get_max_threads.argtypes = []
    _lib.quant_get_max_threads.restype  = c_int
else:
    _TASK_TO_SYMBOL = {}


# ── Public API ───────────────────────────────────────────────────────────────

def is_engine_available() -> bool:
    return _lib is not None


def set_num_threads(n_threads: int) -> int:
    if _lib is None:
        return 1
    _lib.quant_set_num_threads(max(1, int(n_threads)))
    return int(_lib.quant_get_max_threads())


def get_max_threads() -> int:
    if _lib is None:
        return 1
    return int(_lib.quant_get_max_threads())


def run_engine_task(task_type: str, payload: dict) -> dict:
    if _lib is None:
        return {
            "contract_version": CONTRACT_VERSION,
            "trace_id": payload.get("trace_id", "router-fallback"),
            "status": "error",
            "decision": "block",
            "result_summary": {},
            "result_details": {},
            "error": {"code": "engine_unavailable", "message": ENGINE_PROBLEM},
        }
    symbol = _TASK_TO_SYMBOL.get(task_type)
    if symbol is None:
        return {
            "contract_version": CONTRACT_VERSION,
            "trace_id": payload.get("trace_id", "router"),
            "status": "error",
            "decision": "block",
            "result_summary": {},
            "result_details": {},
            "error": {"code": "unsupported_task_type", "message": f"unsupported task_type: {task_type}"},
        }

    raw = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    cap = _INITIAL_BUFFER
    buf = create_string_buffer(cap)
    written = c_int(0)
    rc = int(getattr(_lib, symbol)(c_char_p(raw), buf, cap, byref(written)))
    for _ in range(3):
        if rc != 1:
            break
        # Response larger than the buffer: the engine reported the size it needs.
        # The rerun can come out a few bytes longer (timings are part of the
        # response), so leave headroom instead of allocating exactly that size.
        cap = written.value + written.value // 4 + 4096
        buf = create_string_buffer(cap)
        rc = int(getattr(_lib, symbol)(c_char_p(raw), buf, cap, byref(written)))
    if rc != 0:
        return {
            "contract_version": CONTRACT_VERSION,
            "trace_id": payload.get("trace_id", "router"),
            "status": "error",
            "decision": "block",
            "result_summary": {},
            "result_details": {},
            "error": {"code": f"engine_rc_{rc}", "message": f"{symbol} failed"},
        }
    return json.loads(buf.value.decode("utf-8", errors="replace"))


# ── Domain helpers ────────────────────────────────────────────────────────────

def pricing_bundle(
    spot: float, strike: float, rate: float, vol: float, maturity: float,
    n_paths: int, steps: int, dividend_yield: float, option_type: str = "call",
) -> dict:
    r = run_engine_task("pricing", {
        "spot": spot, "strike": strike, "rate": rate, "vol": vol,
        "maturity": maturity, "n_paths": n_paths, "dividend_yield": dividend_yield,
        "n_steps": int(steps), "option_type": option_type,
    })
    if r.get("status") == "error":
        intrinsic = max(spot - strike, 0.0) if option_type != "put" else max(strike - spot, 0.0)
        return {"mc": intrinsic, "bs": intrinsic, "binomial": intrinsic, "trinomial": intrinsic,
                "mc_std_err": 0.0, "delta_bs": 0.5, "vega_bs": 0.0,
                "mc_minus_bs": 0.0, "binomial_minus_bs": 0.0}
    s = r.get("result_summary", {})
    greeks = s.get("greeks", {})
    err    = s.get("error_decomposition", {})
    return {
        "mc":       s.get("mc",       0.0),
        "bs":       s.get("bs",       0.0),
        "binomial": s.get("binomial", 0.0),
        "trinomial": s.get("trinomial", 0.0),
        "mc_std_err":         s.get("mc_std_err",      0.0),
        "delta_bs":           greeks.get("delta_bs",   0.5),
        "gamma_bs":           greeks.get("gamma_bs",   0.0),
        "theta_bs":           greeks.get("theta_bs",   0.0),
        "vega_bs":            greeks.get("vega_bs",    0.0),
        "rho_bs":             greeks.get("rho_bs",     0.0),
        "mc_minus_bs":        err.get("mc_minus_bs",   0.0),
        "binomial_minus_bs":  err.get("binomial_minus_bs", 0.0),
    }


def pricing_batch(jobs: list[dict]) -> list[dict]:
    n = len(jobs)
    if n == 0:
        return []
    payload: dict = {"n_jobs": n}
    for i, job in enumerate(jobs):
        payload[f"job_{i}_spot"]           = job["spot"]
        payload[f"job_{i}_strike"]         = job["strike"]
        payload[f"job_{i}_rate"]           = job["rate"]
        payload[f"job_{i}_vol"]            = job["vol"]
        payload[f"job_{i}_maturity"]       = job["maturity"]
        payload[f"job_{i}_n_paths"]        = int(job.get("n_paths", 10000))
        payload[f"job_{i}_dividend_yield"] = job.get("dividend_yield", 0.0)
    r = run_engine_task("pricing_batch", payload)
    if r.get("status") == "error":
        return [{"mc": max(j["spot"] - j["strike"], 0.0),
                 "bs": max(j["spot"] - j["strike"], 0.0),
                 "binomial": max(j["spot"] - j["strike"], 0.0)} for j in jobs]
    rows = r.get("result_details", {}).get("flat_rows", [])
    return [{"mc": row.get("mc", 0.0), "bs": row.get("bs", 0.0),
             "binomial": row.get("binomial", 0.0)} for row in rows]


def scenario_bs5(
    spot: float, strike: float, rate: float, vol: float,
    maturity: float, dividend_yield: float,
) -> list[float]:
    r = run_engine_task("scenario", {
        "spot": spot, "strike": strike, "rate": rate, "vol": vol,
        "maturity": maturity, "dividend_yield": dividend_yield,
    })
    if r.get("status") == "error":
        return [max(spot - strike, 0.0)] * 5
    rows = r.get("result_details", {}).get("rows", [])
    return [row.get("bs_price", 0.0) for row in rows[:5]]


def simulation_path(
    model: str, n_steps: int, dt: float,
    sigma: float, kappa: float, theta: float, x0: float,
) -> list[float]:
    r = run_engine_task("simulation", {
        "model": model, "n_steps": n_steps, "dt": dt,
        "sigma": sigma, "kappa": kappa, "theta": theta, "x0": x0,
    })
    if r.get("status") == "error":
        return [x0] * (max(int(n_steps), 1) + 1)
    return r.get("result_details", {}).get("values", [x0])


def stats_normal(mu: float, sigma: float, theta: float, sample_size: int) -> dict:
    r = run_engine_task("stats", {
        "mu": mu, "sigma": sigma, "theta": theta, "sample_size": sample_size,
    })
    if r.get("status") == "error":
        import math
        s = max(sigma, 1e-8)
        exact = {"mgf": math.exp(mu * theta + 0.5 * s * s * theta * theta), "mean": mu, "variance": s * s}
        return {**exact, **{f"{k}_exact": v for k, v in exact.items()}}
    s = r.get("result_summary", {})
    return {k: s.get(k, 0.0) for k in ("mgf", "mean", "variance", "mgf_exact", "mean_exact", "variance_exact")}


def ito_check(function_type: str, theta: float, t: float, n_steps: int) -> dict:
    r = run_engine_task("ito_check", {
        "function_type": function_type, "theta": theta, "t": t, "n_steps": n_steps,
    })
    if r.get("status") == "error":
        return {"function_type": function_type, "value": 0.0,
                "target_expectation": 1.0 if function_type == "exp_martingale" else 0.0,
                "std_error": 0.0, "formula_residual": 0.0, "paths": 0}
    s = r.get("result_summary", {})
    return {"function_type": s.get("function_type", function_type),
            **{k: s.get(k, 0.0) for k in ("value", "target_expectation", "std_error", "formula_residual", "paths")}}


def measure_density_path(mu: float, r: float, sigma: float, t: float, n_steps: int) -> list[float]:
    result = run_engine_task("measure_density", {
        "mu": mu, "r": r, "sigma": sigma, "t": t, "n_steps": n_steps,
    })
    if result.get("status") == "error":
        return [1.0] * (max(int(n_steps), 20) + 1)
    return result.get("result_details", {}).get("density", [1.0])


def pde_solve(
    spot: float, strike: float, rate: float, vol: float, maturity: float,
    dividend_yield: float, s_steps: int, t_steps: int, method: str,
    option_type: str = "call", is_american: bool = False,
) -> dict:
    """Finite-difference price plus grid diagnostics (t_steps_used, psor_iterations, ...)."""
    r = run_engine_task("pde", {
        "spot": spot, "strike": strike, "rate": rate, "vol": vol,
        "maturity": maturity, "dividend_yield": dividend_yield,
        "s_steps": int(s_steps), "t_steps": int(t_steps), "method": method,
        "option_type": option_type, "is_american": is_american,
    })
    if r.get("status") == "error":
        raise ValueError(f"pde failed: {r.get('error', {}).get('message', 'unknown')}")
    return r.get("result_summary", {})


def pde_price(
    spot: float, strike: float, rate: float, vol: float, maturity: float,
    dividend_yield: float, s_steps: int, t_steps: int, method: str,
) -> float:
    r = run_engine_task("pde", {
        "spot": spot, "strike": strike, "rate": rate, "vol": vol,
        "maturity": maturity, "dividend_yield": dividend_yield,
        "s_steps": int(s_steps), "t_steps": int(t_steps), "method": method,
    })
    if r.get("status") == "error":
        return max(spot - strike, 0.0)
    return r.get("result_summary", {}).get("price", max(spot - strike, 0.0))


def measure_compare(
    mu: float, r: float, sigma: float, t: float,
    n_steps: int, n_paths: int, x0: float, preview_len: int = 50,
) -> dict:
    result = run_engine_task("measure_compare", {
        "mu": mu, "r": r, "sigma": sigma, "t": t,
        "n_steps": n_steps, "n_paths": n_paths, "x0": x0,
        "preview_len": preview_len,
    })
    dummy_stats = {"mean": x0, "variance": 0.0, "q05": x0, "q50": x0, "q95": x0}
    if result.get("status") == "error":
        return {"p_stats": dummy_stats, "q_stats": dummy_stats,
                "path_preview": {"P": [x0] * preview_len, "Q": [x0] * preview_len}}
    s  = result.get("result_summary", {})
    rd = result.get("result_details", {})
    return {
        "p_stats": s.get("p_stats", dummy_stats),
        "q_stats": s.get("q_stats", dummy_stats),
        "path_preview": rd.get("path_preview", {"P": [], "Q": []}),
    }


def digital_call_bs(
    spot: float, strike: float, rate: float, vol: float,
    maturity: float, dividend_yield: float,
) -> float:
    r = run_engine_task("pricing", {
        "spot": spot, "strike": strike, "rate": rate, "vol": vol,
        "maturity": maturity, "dividend_yield": dividend_yield,
        "product_type": "digital_call",
    })
    if r.get("status") == "error":
        import math
        if spot <= 0 or strike <= 0 or maturity <= 0 or vol <= 0:
            return math.exp(-rate * maturity) if spot > strike else 0.0
        d2 = (math.log(spot / strike) + (rate - dividend_yield - 0.5 * vol * vol) * maturity) \
             / (vol * math.sqrt(maturity))
        return math.exp(-rate * maturity) * 0.5 * (1.0 + math.erf(d2 / math.sqrt(2.0)))
    return r.get("result_summary", {}).get("bs", 0.0)


def binomial_american(
    spot: float, strike: float, rate: float, vol: float, maturity: float,
    steps: int, dividend_yield: float = 0.0,
) -> float:
    r = run_engine_task("pricing", {
        "spot": spot, "strike": strike, "rate": rate, "vol": vol,
        "maturity": maturity, "dividend_yield": dividend_yield,
        "is_american": True, "n_paths": 2000,
    })
    if r.get("status") == "error":
        return max(spot - strike, 0.0)
    s = r.get("result_summary", {})
    return s.get("american") or s.get("binomial", max(spot - strike, 0.0))


def delta_hedge_pnl_distribution(
    spot: float, strike: float, rate: float, vol: float, maturity: float,
    n_rebalances: int, n_paths: int, n_bins: int = 40,
) -> dict:
    r = run_engine_task("hedging", {
        "spot": spot, "strike": strike, "rate": rate, "vol": vol,
        "maturity": maturity, "n_rebalances": n_rebalances,
        "n_paths": n_paths, "n_bins": n_bins,
    })
    if r.get("status") == "error":
        return {"mean": 0.0, "std": 0.0, "q05": 0.0, "q50": 0.0, "q95": 0.0,
                "histogram": {"edges": [], "counts": []}}
    s  = r.get("result_summary", {})
    rd = r.get("result_details", {})
    return {
        "mean": s.get("pnl_mean", 0.0),
        "std":  s.get("pnl_std",  0.0),
        "q05":  s.get("pnl_q05",  0.0),
        "q50":  s.get("pnl_q50",  0.0),
        "q95":  s.get("pnl_q95",  0.0),
        "histogram": rd.get("histogram", {"edges": [], "counts": []}),
    }


def delta_hedge_strategy_compare(
    spot: float, strike: float, rate: float, vol: float, maturity: float,
    n_rebalances: int, n_paths: int,
    transaction_cost_bps: float = 5.0,
    rebalance_threshold: float = 0.02,
    vol_mismatch_mult: float = 1.15,
) -> dict:
    labels = ["discrete_delta", "with_transaction_cost", "threshold_rebalance", "vol_mismatch"]
    r = run_engine_task("hedging", {
        "spot": spot, "strike": strike, "rate": rate, "vol": vol,
        "maturity": maturity, "n_rebalances": n_rebalances, "n_paths": n_paths,
        "transaction_cost_bps": transaction_cost_bps,
        "rebalance_threshold": rebalance_threshold,
        "vol_mismatch_mult": vol_mismatch_mult,
    })
    base = {"mean": 0.0, "std": 0.0, "q05": 0.0, "q50": 0.0, "q95": 0.0,
            "turnover": 0.0, "transaction_cost": 0.0, "var95": 0.0, "es95": 0.0}
    if r.get("status") == "error":
        return {k: dict(base) for k in labels}
    compare = r.get("result_details", {}).get("strategy_compare", {})
    return {k: compare.get(k, dict(base)) for k in labels}


def vol_surface_interp(
    surface: list[dict], spot: float, target_strike: float, target_expiry: float,
) -> tuple[float, str] | None:
    """Returns (iv, method) or None if surface is empty."""
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
    summary = r.get("result_summary", {})
    iv = summary.get("iv")
    method = summary.get("method", "cpp_bilinear")
    if iv is None or iv <= 0:
        return None
    return float(iv), str(method)


def greek_surface(
    strike: float, rate: float, vol: float, dividend_yield: float,
    spot_min: float, spot_max: float,
    mat_min: float = 0.1, mat_max: float = 3.0,
    n_spots: int = 21, n_mats: int = 11,
    greek: str = "delta",
) -> dict:
    r = run_engine_task("greek_surface", {
        "strike": strike, "rate": rate, "vol": vol, "dividend_yield": dividend_yield,
        "spot_min": spot_min, "spot_max": spot_max,
        "mat_min": mat_min, "mat_max": mat_max,
        "n_spots": n_spots, "n_mats": n_mats, "greek": greek,
    })
    if r.get("status") == "error":
        return {"greek": greek, "spots": [], "maturities": [], "grid": [],
                "grid_min": 0.0, "grid_max": 1.0}
    s  = r.get("result_summary", {})
    rd = r.get("result_details", {})
    return {
        "greek":      s.get("greek",    greek),
        "spots":      rd.get("spots",      []),
        "maturities": rd.get("maturities", []),
        "grid":       rd.get("grid",       []),
        "grid_min":   s.get("grid_min",  0.0),
        "grid_max":   s.get("grid_max",  1.0),
        "n_spots":    s.get("n_spots",    0),
        "n_mats":     s.get("n_mats",     0),
    }


def multi_leg(
    spot: float, rate: float, vol: float, maturity: float,
    legs: list[dict],
    dividend_yield: float = 0.0,
    n_paths: int = 10000,
) -> dict:
    """legs: list of {option_type, strike, quantity}"""
    r = run_engine_task("multi_leg", {
        "spot": spot, "rate": rate, "vol": vol, "maturity": maturity,
        "dividend_yield": dividend_yield, "n_paths": n_paths, "legs": legs,
    })
    if r.get("status") == "error":
        return {"net_bs_price": 0.0, "net_mc_price": 0.0, "net_binomial_price": 0.0, "net_delta": 0.0,
                "net_vega": 0.0, "strategy_hint": "error", "legs": []}
    s  = r.get("result_summary", {})
    rd = r.get("result_details", {})
    return {
        "net_bs_price":  s.get("net_bs_price",  0.0),
        "net_mc_price":  s.get("net_mc_price",  0.0),
        "net_binomial_price": s.get("net_binomial_price", 0.0),
        "net_delta":     s.get("net_delta",     0.0),
        "net_vega":      s.get("net_vega",      0.0),
        "strategy_hint": s.get("strategy_hint", ""),
        "n_legs":        s.get("n_legs",        0),
        "legs":          rd.get("legs",         []),
    }


def implied_vol_single(
    market_price: float, spot: float, strike: float,
    rate: float, maturity: float, dividend_yield: float = 0.0,
) -> dict:
    r = run_engine_task("implied_vol", {
        "market_price": market_price, "spot": spot, "strike": strike,
        "rate": rate, "maturity": maturity, "dividend_yield": dividend_yield,
    })
    if r.get("status") == "error":
        return {"implied_vol": -1.0, "converged": False, "final_error": 0.0}
    s = r.get("result_summary", {})
    return {
        "implied_vol":  s.get("implied_vol",  -1.0),
        "converged":    s.get("converged",    False),
        "final_error":  s.get("final_error",  0.0),
    }


def implied_vol_batch(
    market_prices: list[float], strikes: list[float], expiries: list[float],
    spot: float, rate: float, dividend_yield: float = 0.0,
) -> dict:
    r = run_engine_task("implied_vol_batch", {
        "spot": spot, "rate": rate, "dividend_yield": dividend_yield,
        "market_prices": market_prices, "strikes": strikes, "expiries": expiries,
    })
    if r.get("status") == "error":
        return {"ivs": [-1.0] * len(market_prices), "converged": [False] * len(market_prices),
                "n_converged": 0, "convergence_rate": 0.0}
    s  = r.get("result_summary", {})
    rd = r.get("result_details", {})
    return {
        "ivs":              rd.get("ivs",       []),
        "converged":        rd.get("converged", []),
        "n_converged":      s.get("n_converged", 0),
        "convergence_rate": s.get("convergence_rate", 0.0),
    }


def black_batch(is_call: list[bool], forwards: list[float], strikes: list[float], maturities: list[float],
                rate: float, *, vols: list[float] | None = None, prices: list[float] | None = None,
                spots: list[float] | None = None) -> dict:
    """Black on each option's forward, discounted at rate: the trading engine's own pricing. With vols:
    {"prices", "delta", "gamma", "theta", "vega"} (Greeks to spots, the forward when absent); with
    prices: {"ivs"} (0: none). Raises RuntimeError when the engine is missing or refuses."""
    payload = {"rate": rate, "is_call": is_call, "forwards": forwards, "strikes": strikes, "maturities": maturities}
    payload.update({"vols": vols} if vols is not None else {"prices": prices})
    if spots is not None:
        payload["spots"] = spots
    r = run_engine_task("black_batch", payload)
    if r.get("status") == "error":
        raise RuntimeError(f"black_batch failed: {(r.get('error') or {}).get('message') or r.get('error')}")
    return r.get("result_details", {})


def heston_price(
    spot: float, strike: float, rate: float, maturity: float,
    v0: float = 0.04, kappa: float = 1.5, theta: float = 0.04,
    xi: float = 0.5, rho: float = -0.7,
) -> float:
    r = run_engine_task("heston_price", {
        "spot": spot, "strike": strike, "rate": rate, "maturity": maturity,
        "v0": v0, "kappa": kappa, "theta": theta, "xi": xi, "rho": rho,
    })
    if r.get("status") == "error":
        return max(spot - strike, 0.0)
    return r.get("result_summary", {}).get("heston_price", 0.0)


def heston_calibrate(
    spot: float, rate: float,
    market_strikes: list[float], market_maturities: list[float], market_prices: list[float],
    dividend_yield: float = 0.0,
    init_v0: float = 0.04, init_kappa: float = 1.5, init_theta: float = 0.04,
    init_xi: float = 0.5, init_rho: float = -0.7,
    max_iter: int = 500,
) -> dict:
    r = run_engine_task("heston_calibrate", {
        "spot": spot, "rate": rate, "dividend_yield": dividend_yield,
        "market_strikes": market_strikes, "market_maturities": market_maturities,
        "market_prices": market_prices,
        "init_v0": init_v0, "init_kappa": init_kappa, "init_theta": init_theta,
        "init_xi": init_xi, "init_rho": init_rho, "max_iter": max_iter,
    })
    default = {"v0": init_v0, "kappa": init_kappa, "theta": init_theta,
               "xi": init_xi, "rho": init_rho, "rmse": 999.0,
               "max_abs_error": 999.0, "iterations": 0, "converged": False,
               "model_prices": [], "residuals": []}
    if r.get("status") == "error":
        return default
    s  = r.get("result_summary", {})
    rd = r.get("result_details", {})
    return {
        "v0":            s.get("v0",            init_v0),
        "kappa":         s.get("kappa",         init_kappa),
        "theta":         s.get("theta",         init_theta),
        "xi":            s.get("xi",            init_xi),
        "rho":           s.get("rho",           init_rho),
        "rmse":          s.get("rmse",          999.0),
        "max_abs_error": s.get("max_abs_error", 999.0),
        "iterations":    s.get("iterations",    0),
        "converged":     s.get("converged",     False),
        "model_prices":  rd.get("model_prices", []),
        "residuals":     rd.get("residuals",    []),
    }


def screener(request: dict) -> dict:
    """Run the C++ strategy screener.

    `request` follows the quant_run_screener_json contract (chain + filters + rank).
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
    rows = []
    prev_err = prev_tri = None
    for steps in step_ladder:
        st = max(2, int(steps))
        s = run_engine_task("pricing", {
            "spot": spot, "strike": strike, "rate": rate, "vol": vol,
            "maturity": maturity, "dividend_yield": dividend_yield,
            "n_paths": 100, "n_steps": st, "option_type": option_type,
        }).get("result_summary", {})
        bs_ref = s.get("bs", 0.0)
        binomial, trinomial = s.get("binomial", 0.0), s.get("trinomial", 0.0)
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
