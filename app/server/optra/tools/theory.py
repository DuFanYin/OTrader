"""Stochastic-calculus checks and the validation gate: moments, Itô, simulated paths, change of measure."""

from optra.schemas.tools import (
    ItoCheckRequest,
    MeasureChangeRequest,
    MeasureCompareRequest,
    SimulationRequest,
    StatsRequest,
    ValidationGateRequest,
)
from optra.services.analytics import summarize_density
from optra.infra.numerics import (
    ito_check as engine_ito_check,
    measure_compare as engine_measure_compare,
    measure_density_path,
    stats_normal,
)
from optra.tools.base import Result, Tool, engine_task


def normal_moments(req: StatsRequest) -> Result:
    return Result(summary={**stats_normal(req.mu, req.sigma, req.theta, req.sample_size), "sample_size": req.sample_size})


def ito_check(req: ItoCheckRequest) -> Result:
    return Result(summary=engine_ito_check(req.function_type, req.theta, req.t, req.n_steps))


def simulate_path(req: SimulationRequest) -> Result:
    return engine_task("simulation", req.model_dump())


def measure_density(req: MeasureChangeRequest) -> Result:
    density = measure_density_path(req.mu, req.r, req.sigma, req.t, req.n_steps)
    theta = (req.mu - req.r) / max(req.sigma, 1e-8)
    return Result(summary={"theta_market_price_of_risk": theta, "final_density": density[-1],
                           **summarize_density(density, req.t)},
                  details={"density_preview": density[:30]})


def measure_compare(req: MeasureCompareRequest) -> Result:
    sigma = max(req.sigma, 1e-10)
    result = engine_measure_compare(req.mu, req.r, sigma, req.t, int(req.n_steps), int(req.n_paths), req.x0, preview_len=50)
    p, q = result["p_stats"], result["q_stats"]
    mean_shift = p["mean"] - q["mean"]
    return Result(
        summary={
            "theta_market_price_of_risk": (req.mu - req.r) / sigma,
            "drift_p": req.mu, "drift_q": req.r, "drift_diff": req.mu - req.r,
            "terminal_mean_p": p["mean"],
            "terminal_mean_q": q["mean"],
            "q95_gap": p["q95"] - q["q95"],
            "var_gap": p["variance"] - q["variance"],
            "path_dispersion_gap": (p["q95"] - p["q05"]) - (q["q95"] - q["q05"]),
            "mean_shift": mean_shift,
            "mean_shift_pct": mean_shift / max(abs(q["mean"]), 1e-12),
            "drift_ratio": req.mu / req.r if abs(req.r) > 1e-12 else None,
            "variance_ratio": p["variance"] / max(abs(q["variance"]), 1e-12),
        },
        details={
            "path_preview": result["path_preview"],
            "distribution_compare": {"P": p, "Q": q},
            "distribution_rows": [{"measure": m, **{k: s[k] for k in ("mean", "variance", "q05", "q50", "q95")}}
                                  for m, s in (("P", p), ("Q", q))],
        },
        notes=["p_vs_q_cpp"],
    )


def validate(req: ValidationGateRequest) -> Result:
    gate = engine_task("validation_gate", req.model_dump())
    failed = [r for r in gate.details["rows"] if r["status"] == "fail"]
    gate.warnings += [f"{r['capability']}.{r['metric']} = {r['value']:.4g} is over its threshold {r['threshold']:.4g}" for r in failed]
    return gate


THEORY = "Part of the lab's checks of the theory against simulation. "

TOOLS = [
    Tool("normal_moments", "Sample moments of a normal",
         THEORY + "Draws sample_size values from N(mu, sigma²) and returns their sample mean, variance and moment-generating "
         "function E[exp(theta X)] next to the exact values (mean_exact, variance_exact, mgf_exact).",
         StatsRequest, normal_moments, "/lab/stats/run", example={"mu": 0.05, "sigma": 0.2, "sample_size": 100000}),
    Tool("ito_check", "Itô's formula check",
         THEORY + "For f(W) = exp(theta W − theta² t/2), W² − t or W³: E[f(W_t)] by Monte Carlo against its exact value "
         "(value, target_expectation, std_error), and Itô's formula on discrete paths of n_steps (formula_residual: the "
         "RMS gap relative to f(W_t) − f(0), which shrinks like sqrt(dt)).",
         ItoCheckRequest, ito_check, "/lab/ito/run", example={"function_type": "w2_minus_t", "n_steps": 1000}),
    Tool("simulate_path", "Simulate a path",
         "Simulates one path of Brownian motion or a Vasicek (mean-reverting) process. Returns the final, lowest and highest "
         "values and the path in result_details.values_preview (at most about 200 points of it).",
         SimulationRequest, simulate_path, "/lab/simulation/run", example={"model": "vasicek", "n_steps": 250, "dt": 0.004}),
    Tool("measure_density", "Radon-Nikodym density",
         THEORY + "The Radon-Nikodym density path dQ/dP for a geometric Brownian motion with drift mu under P and r under Q, "
         "and the market price of risk theta = (mu − r) / sigma.",
         MeasureChangeRequest, measure_density, "/lab/measure/run", example={"mu": 0.08, "r": 0.03, "sigma": 0.2}),
    Tool("measure_compare", "Physical against risk-neutral",
         THEORY + "Simulates a geometric Brownian motion under P (drift mu) and Q (drift r) from the same shocks and compares "
         "the terminal distributions (mean, variance, quantiles) and the drift and variance ratios.",
         MeasureCompareRequest, measure_compare, "/lab/measure/compare", example={"mu": 0.08, "r": 0.03, "sigma": 0.2}),
    Tool("validate", "Validation gate",
         "Runs the chosen checks (pick_stats, pick_ito, pick_simulation, pick_lattice) with their parameters, each against a "
         "threshold, and says what to change when one fails. Use it before trusting a run, or to see whether a numerical "
         "setting is good enough. Returns result_summary (gate_decision go / warning / blocked, checks_failed) and "
         "result_details.rows: per check its value, threshold and status, what to change (action) and, where an input "
         "cures it, that input (param) with its current_value and proposed_value.",
         ValidationGateRequest, validate, "/lab/validation/gate",
         example={"spot": 100.0, "strike": 100.0, "rate": 0.03, "vol": 0.2, "maturity": 0.5,
                  "pick_stats": True, "pick_ito": True, "pick_lattice": True}),
]
