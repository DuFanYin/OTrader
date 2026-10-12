"""Shared analytics helpers for route-level aggregation."""

from math import log, sqrt


def quantile(sorted_vals: list[float], q: float) -> float:
    if not sorted_vals:
        return 0.0
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    pos = (len(sorted_vals) - 1) * q
    lo = int(pos)
    hi = min(lo + 1, len(sorted_vals) - 1)
    w = pos - lo
    return sorted_vals[lo] * (1.0 - w) + sorted_vals[hi] * w


def stdev_population(vals: list[float]) -> float:
    if not vals:
        return 0.0
    mean = sum(vals) / len(vals)
    return sqrt(sum((x - mean) ** 2 for x in vals) / len(vals))


def convergence_summary(rows: list[dict], bs_ref: float) -> tuple[dict, dict]:
    best_abs = min(r["abs_error"] for r in rows)
    worst_abs = max(r["abs_error"] for r in rows)
    best_steps = next(r["steps"] for r in rows if r["abs_error"] == best_abs)
    first_err = rows[0]["abs_error"] if rows else 0.0
    last_err = rows[-1]["abs_error"] if rows else 0.0
    first_to_best_improvement = first_err - best_abs
    improvement_ratio = (first_err / last_err) if last_err > 1e-12 else None
    last_two_improvement_ratio = None
    if len(rows) >= 2 and rows[-2]["abs_error"] > 0:
        last_two_improvement_ratio = rows[-1]["abs_error"] / rows[-2]["abs_error"]

    monotonicity_break_count = 0
    for i in range(1, len(rows)):
        if rows[i]["abs_error"] > rows[i - 1]["abs_error"]:
            monotonicity_break_count += 1

    log_slope = None
    pos_rows = [r for r in rows if r["steps"] > 0 and r["abs_error"] > 0]
    if len(pos_rows) >= 2:
        x1, y1 = pos_rows[0]["steps"], pos_rows[0]["abs_error"]
        x2, y2 = pos_rows[-1]["steps"], pos_rows[-1]["abs_error"]
        if x1 > 0 and x2 > 0 and x2 != x1:
            log_slope = (log(y2) - log(y1)) / (log(x2) - log(x1))

    summary = {
        "bs_ref": bs_ref,
        "ladder_size": len(rows),
        "best_abs_error": best_abs,
        "worst_abs_error": worst_abs,
        "best_steps": best_steps,
        "last_error": last_err,
        "improvement_ratio": improvement_ratio,
        "log_slope": log_slope,
        "first_to_best_improvement": first_to_best_improvement,
        "last_two_improvement_ratio": last_two_improvement_ratio,
        "monotonicity_break_count": monotonicity_break_count,
    }
    details = {"rows": rows, "curve_points": [{"x": r["steps"], "y": r["abs_error"]} for r in rows]}
    return summary, details


def rank_benchmark_rows(rows: list[dict]) -> list[dict]:
    runtime_sorted = sorted(rows, key=lambda x: x["runtime_ms"])
    runtime_rank = {r["method"]: i + 1 for i, r in enumerate(runtime_sorted)}
    acc_sorted = sorted(rows, key=lambda x: x["accuracy_abs_error"])
    acc_rank = {r["method"]: i + 1 for i, r in enumerate(acc_sorted)}
    for r in rows:
        r["runtime_rank"] = runtime_rank[r["method"]]
        r["accuracy_rank"] = acc_rank[r["method"]]
        r["efficiency"] = 1.0 / (r["runtime_ms"] * r["accuracy_abs_error"] + 1e-12)
    eff_sorted = sorted(rows, key=lambda x: x["efficiency"], reverse=True)
    eff_rank = {r["method"]: i + 1 for i, r in enumerate(eff_sorted)}
    for r in rows:
        r["efficiency_rank"] = eff_rank[r["method"]]
    return rows


def summarize_density(density: list[float], t: float) -> dict:
    dt = t / max(len(density) - 1, 1)
    density_integral_proxy = sum(density) * dt
    density_mean = sum(density) / max(len(density), 1)
    density_std = stdev_population(density)
    density_cv = density_std / max(abs(density_mean), 1e-12)
    q_vals = sorted(density)
    q05 = quantile(q_vals, 0.05)
    q95 = quantile(q_vals, 0.95)
    tail_ratio = q95 / max(q05, 1e-12)
    autocorr_lag1 = 0.0
    if len(density) > 1 and density_std > 1e-12:
        cov = 0.0
        for i in range(1, len(density)):
            cov += (density[i] - density_mean) * (density[i - 1] - density_mean)
        autocorr_lag1 = cov / max((len(density) - 1) * (density_std ** 2), 1e-12)
    return {
        "density_count": len(density),
        "density_integral_proxy": density_integral_proxy,
        "density_cv": density_cv,
        "density_tail_ratio_q95_q05": tail_ratio,
        "density_autocorr_lag1": autocorr_lag1,
    }


def summarize_hedging_distribution(dist: dict, spot: float, n_rebalances: int) -> dict:
    pnl_iqr = dist["q95"] - dist["q05"]
    left_tail = dist["q50"] - dist["q05"]
    right_tail = dist["q95"] - dist["q50"]
    tail_skew_proxy = ((dist["q95"] - dist["q50"]) - (dist["q50"] - dist["q05"])) / max(pnl_iqr, 1e-12)
    normalized_std = dist["std"] / max(spot, 1e-12)
    tail_ratio_q95_q05 = (dist["q95"] / dist["q05"]) if abs(dist["q05"]) > 1e-12 else None
    scaling_proxy = dist["std"] * sqrt(max(n_rebalances, 1))
    return {
        "pnl_iqr": pnl_iqr,
        "tail_span": pnl_iqr,
        "left_tail": left_tail,
        "right_tail": right_tail,
        "tail_skew_proxy": tail_skew_proxy,
        "normalized_std": normalized_std,
        "tail_ratio_q95_q05": tail_ratio_q95_q05,
        "scaling_proxy_std_sqrt_n": scaling_proxy,
    }


def build_batch_grid(
    base: dict,
    n_jobs: int,
    spot_shock: float,
) -> list[dict]:
    """Build a symmetric spot/vol shock grid from a single base spec.

    Each job shifts spot by ``i * spot_shock`` and vol by half that amount,
    centred on the middle index so the grid is symmetric around the base params.
    """
    center = (n_jobs - 1) / 2
    return [
        {
            **base,
            "spot": base["spot"] * max(0.01, 1 + (i - center) * spot_shock),
            "vol":  base["vol"]  * max(0.05, 1 + (i - center) * spot_shock * 0.5),
        }
        for i in range(n_jobs)
    ]
