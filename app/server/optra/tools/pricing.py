"""Pricing tools: one option every way, batches and grids, scenario sweeps, finite differences, lattice convergence and the
cross-method benchmark."""

from time import perf_counter

from optra.schemas.tools import (
    BenchmarkRequest,
    ConvergenceRequest,
    PdeRequest,
    PricingBatchGridRequest,
    PricingBatchRequest,
    PricingRequest,
)
from optra.services.analytics import convergence_summary, rank_benchmark_rows
from optra.infra.numerics import lattice_prices, pde_solve, pricing_bundle, run_convergence_steps
from optra.tools.base import Result, Tool, engine_task

OPTION = {"spot": 100.0, "strike": 105.0, "rate": 0.03, "vol": 0.2, "maturity": 0.5}


def price_option(req: PricingRequest) -> Result:
    return engine_task("pricing", req.model_dump())


def price_batch(req: PricingBatchRequest) -> Result:
    keys = ("spot", "strike", "rate", "vol", "maturity", "n_paths", "dividend_yield")
    return engine_task("pricing_batch", {"jobs": [{k: getattr(job, k) for k in keys} for job in req.jobs]})


def price_grid(req: PricingBatchGridRequest) -> Result:
    return engine_task("pricing_batch_grid", {**req.base.model_dump(), "n_jobs": req.n_jobs, "spot_shock": req.spot_shock})


def scenario_sweep(req: PricingRequest) -> Result:
    result = engine_task("scenario", req.model_dump())
    if req.is_american:
        result.warnings.append("the scenario sweep prices the European option with Black-Scholes, without early exercise")
    return result


def pde_price(req: PdeRequest) -> Result:
    pde = pde_solve(req.spot, req.strike, req.rate, req.vol, req.maturity, req.dividend_yield,
                    int(req.s_steps), int(req.t_steps), req.method, req.option_type, req.is_american)
    # Reference: Black-Scholes for European; a 1000-step binomial tree for American.
    ref = lattice_prices(req.spot, req.strike, req.rate, req.vol, req.maturity, req.dividend_yield, [1000],
                         req.option_type, req.is_american)
    ref_name = "binomial_american" if req.is_american else "bs"
    ref_price = ref["binomial"][0] if req.is_american else ref["bs"]
    price = pde["price"]
    t_used = pde["t_steps_used"]
    grid_points = req.s_steps * t_used
    notes = ["pde_cpp"]
    if pde["stability_refined"]:
        notes.append(f"explicit scheme refined to {t_used} time steps for stability")
    return Result(summary={
        "price": price,
        "method": req.method,
        "option_type": req.option_type,
        "is_american": req.is_american,
        "s_steps": req.s_steps,
        "t_steps": req.t_steps,
        "t_steps_used": t_used,
        "stability_refined": pde["stability_refined"],
        "psor_iterations": pde["psor_iterations"],
        "grid_points": grid_points,
        "s_t_aspect_ratio": req.s_steps / max(t_used, 1),
        "grid_density_per_maturity": grid_points / max(req.maturity, 1e-12),
        "reference_method": ref_name,
        "reference_price": ref_price,
        "price_vs_bs_gap": (price - ref_price) if ref_price is not None else None,
    }, notes=notes)


def lattice_convergence(req: ConvergenceRequest) -> Result:
    rows = run_convergence_steps(req.spot, req.strike, req.rate, req.vol, req.maturity, req.dividend_yield,
                                 req.step_ladder, req.option_type)
    bs_ref = rows[0]["bs_ref"] if rows else 0.0
    summary, details = convergence_summary(rows, bs_ref)
    # Same diagnostics for the trinomial lattice, on its own error column.
    tri_rows = [{**r, "abs_error": r["trinomial_abs_error"]} for r in rows]
    tri_summary, tri_details = convergence_summary(tri_rows, bs_ref)
    summary["option_type"] = req.option_type
    summary["trinomial"] = {k: tri_summary[k] for k in (
        "best_abs_error", "worst_abs_error", "best_steps", "last_error",
        "improvement_ratio", "log_slope", "monotonicity_break_count")}
    details["trinomial_curve_points"] = tri_details["curve_points"]
    return Result(summary=summary, details=details, notes=["binomial_to_bs"])


def benchmark_methods(req: BenchmarkRequest) -> Result:
    p = req.pricing
    steps = max(10, int(p.maturity * 250))
    t0 = perf_counter()
    bundle = pricing_bundle(p.spot, p.strike, p.rate, p.vol, p.maturity, p.n_paths, steps, p.dividend_yield, p.option_type)
    mc_ms = (perf_counter() - t0) * 1000.0
    t1 = perf_counter()
    pde = pde_solve(p.spot, p.strike, p.rate, p.vol, p.maturity, p.dividend_yield,
                    req.pde_s_steps, req.pde_t_steps, req.pde_method, p.option_type)
    pde_ms = (perf_counter() - t1) * 1000.0

    # MC, BS, binomial and trinomial come from one engine call; MC dominates its runtime.
    rows = [
        {"method": "mc",        "price": bundle["mc"],        "runtime_ms": mc_ms},
        {"method": "bs",        "price": bundle["bs"],        "runtime_ms": 0.0},
        {"method": "binomial",  "price": bundle["binomial"],  "runtime_ms": 0.0},
        {"method": "trinomial", "price": bundle["trinomial"], "runtime_ms": 0.0},
        {"method": "pde",       "price": pde["price"],       "runtime_ms": pde_ms},
    ]
    baseline = next(r["price"] for r in rows if r["method"] == req.benchmark_baseline)
    for r in rows:
        ae = abs(r["price"] - baseline)
        re = ae / max(abs(baseline), 1e-10)
        r["accuracy_abs_error"] = ae
        r["accuracy_rel_error"] = re
        r["stability"] = "good" if re < 0.03 else ("ok" if re < 0.08 else "check")
    rank_benchmark_rows(rows)
    ranked = sorted(rows, key=lambda x: (x["accuracy_rel_error"], x["runtime_ms"]))
    return Result(
        summary={"baseline_method": req.benchmark_baseline, "baseline_price": baseline,
                 "winner_method": ranked[0]["method"], "winner_rel_error": ranked[0]["accuracy_rel_error"]},
        details={"rows": ranked, "chart_rows": ranked},
        notes=["cross_method"],
    )


TOOLS = [
    Tool("price_option", "Price an option every way",
         "Prices one European (or American) call or put with Black-Scholes, Monte Carlo, binomial and trinomial trees at "
         "once, with all five Black-Scholes Greeks. Use it for any single option price or Greek; compare the methods to see "
         "the numerical error. Returns result_summary.bs (closed form), mc with mc_std_err and a 95% interval, binomial, "
         "trinomial, greeks (delta_bs, gamma_bs, theta_bs per year, vega_bs per 1.00 of vol, rho_bs), method_spread, and for "
         "American options american_methods (binomial, trinomial, pde_psor, lsm) and the early_exercise_premium.",
         PricingRequest, price_option, "/lab/pricing/run", example=OPTION),
    Tool("price_batch", "Price many options",
         "Prices up to 500 options in one call, each every way, with per-method statistics over the batch (mean, spread). "
         "Use it to price a list of strikes or maturities at once. Returns result_details.rows, one per option.",
         PricingBatchRequest, price_batch, "/lab/pricing/batch",
         example={"jobs": [{**OPTION, "strike": k} for k in (95.0, 100.0, 105.0)]}),
    Tool("price_grid", "Price a spot × vol grid",
         "Prices a grid of spot and vol shocks around one option (spot steps of spot_shock), every way. Use it to see how "
         "the price and the methods' agreement move with spot and vol. Returns result_details.rows and per-method statistics.",
         PricingBatchGridRequest, price_grid, "/lab/pricing/batch/grid", example={"base": OPTION, "n_jobs": 9}),
    Tool("scenario_sweep", "Scenario sweep",
         "Reprices a European call or put with Black-Scholes under five shocks: base, spot +10%, spot −10%, vol +5 points, "
         "rate +1 point. Use it for a quick sensitivity table. Returns result_details.rows and tornado_points with the change against the base.",
         PricingRequest, scenario_sweep, "/lab/scenario/run", example=OPTION),
    Tool("pde_price", "Finite-difference price",
         "Prices a European or American option on a finite-difference grid (Crank-Nicolson, implicit or explicit; American "
         "by PSOR) and compares it with a reference (Black-Scholes, or a 1000-step binomial for American). Use it to study "
         "grid error or price American options. Returns result_summary.price, reference_price and price_vs_bs_gap.",
         PdeRequest, pde_price, "/lab/pde/run", example={**OPTION, "is_american": True, "option_type": "put"}),
    Tool("lattice_convergence", "Lattice convergence",
         "Prices an option with binomial and trinomial trees at a ladder of step counts and measures how the error against "
         "Black-Scholes falls (log_slope ≈ −1 means first order). Use it to choose a step count or show convergence. "
         "Returns result_summary (best_abs_error, log_slope, trinomial) and the curves in result_details.",
         ConvergenceRequest, lattice_convergence, "/lab/convergence/run", example=OPTION),
    Tool("benchmark_methods", "Benchmark the methods",
         "Prices one option with Monte Carlo, Black-Scholes, binomial, trinomial and finite differences, and ranks them by "
         "accuracy against a baseline and by runtime. Use it to compare methods. Returns result_details.rows (price, "
         "accuracy_abs_error, runtime_ms, stability) and the winner.",
         BenchmarkRequest, benchmark_methods, "/lab/benchmark/run", cost="seconds", example={"pricing": OPTION}),
]
