"""Diagnostics of the BTC implied-vol surface (the iv_surface tool): the ATM term structure, the smiles, the IV at a strike
and maturity, the local skew and curvature, and a moneyness × maturity grid with its burst and suppressed zones."""

from __future__ import annotations

import math

from optra.infra.numerics import vol_surface_interp
from optra.services.market_data import fetch_iv_smile_slice, fetch_iv_surface

GRID_MONEYNESS = (0.75, 1.25)    # the grid's strikes, as a share of spot
GRID_MATURITY = (0.4, 1.8)       # the grid's maturities, as a multiple of the target maturity
CONFIDENT_EXPIRIES = 8           # expiries for full marks on coverage


def _expiry(row: dict) -> str:
    return row.get("expiry", str(round(row["years"], 4)))


def _interpolate(surface: list[dict], spot: float, strike: float, years: float) -> tuple[float | None, str]:
    """The engine's bilinear IV at (strike, years), and how it got it."""
    r = vol_surface_interp(surface=surface, spot=spot, target_strike=strike, target_expiry=years)
    return (float(r[0]), str(r[1])) if r is not None else (None, "unavailable")


def term_structure(surface: list[dict], spot: float) -> list[dict]:
    """Per expiry, the row whose strike is nearest spot, in expiry order."""
    atm: dict[str, dict] = {}
    for row in surface:
        e = _expiry(row)
        if e not in atm or abs(row["strike"] - spot) < abs(atm[e]["strike"] - spot):
            atm[e] = row
    return sorted(atm.values(), key=lambda r: r["years"])


def smiles_by_expiry(surface: list[dict], spot: float) -> dict[str, list[dict]]:
    smiles: dict[str, list[dict]] = {}
    for row in surface:
        smiles.setdefault(_expiry(row), []).append({
            "instrument": row.get("instrument", ""),
            "strike": row["strike"],
            "years": row["years"],
            "log_moneyness": round(math.log(max(row["strike"], 1e-8) / max(spot, 1e-8)), 6),
            "iv": row["iv"],
            "iv_pct": row["iv_pct"],
        })
    return smiles


def skew_and_curvature(smile: list[dict]) -> tuple[float | None, float | None]:
    """The smile's slope in log-moneyness (least squares) and its second difference at the middle strike."""
    if len(smile) < 3:
        return None, None
    xs, ys = [r["log_moneyness"] for r in smile], [r["iv"] for r in smile]
    x_mean, y_mean = sum(xs) / len(xs), sum(ys) / len(ys)
    var_x = sum((x - x_mean) ** 2 for x in xs)
    skew = sum((x - x_mean) * (y - y_mean) for x, y in zip(xs, ys)) / var_x if var_x > 1e-12 else 0.0
    mid = len(smile) // 2
    return skew, smile[mid - 1]["iv"] - 2 * smile[mid]["iv"] + smile[mid + 1]["iv"]


def grid(surface: list[dict], spot: float, maturity: float, k_steps: int, t_steps: int) -> list[dict]:
    """The interpolated IV over moneyness × maturity, maturity by maturity."""
    t_min = max(1.0 / 365.0, maturity * GRID_MATURITY[0])
    t_max = max(t_min + 1e-6, maturity * GRID_MATURITY[1])
    k_lo, k_hi = GRID_MONEYNESS
    rows = []
    for t in (t_min + (t_max - t_min) * i / (t_steps - 1) for i in range(t_steps)):
        for k in (k_lo + (k_hi - k_lo) * i / (k_steps - 1) for i in range(k_steps)):
            iv, method = _interpolate(surface, spot, spot * k, t)
            iv = iv or 0.0
            rows.append({"k_ratio": round(k, 4), "strike": round(spot * k, 2), "years": round(t, 6),
                         "iv": round(iv, 6), "iv_pct": round(iv * 100, 3), "zone": "normal", "method": method})
    return rows


def mark_zones(rows: list[dict], lower_q: float, upper_q: float) -> tuple[float, float]:
    """Marks the grid's IVs at or above the upper quantile burst_high and at or below the lower one suppressed_low;
    returns the two thresholds."""
    ivs = sorted(r["iv"] for r in rows)
    if not ivs:
        return 0.0, 0.0
    low = ivs[max(0, int((len(ivs) - 1) * lower_q))]
    high = ivs[min(len(ivs) - 1, int((len(ivs) - 1) * upper_q))]
    for r in rows:
        r["zone"] = "burst_high" if r["iv"] >= high else "suppressed_low" if r["iv"] <= low else "normal"
    return low, high


def confidence(term: list[dict], rows: list[dict]) -> tuple[float, str]:
    """0–1 from how many expiries the surface has and how smooth the grid is."""
    ivs = [r["iv"] for r in rows if r["iv"] > 0]
    spread = max(ivs) - min(ivs) if ivs else 0.0
    score = max(0.0, min(1.0, 0.65 * min(1.0, len(term) / CONFIDENT_EXPIRIES) + 0.35 / (1.0 + spread)))
    return score, "high" if score >= 0.8 else "medium" if score >= 0.55 else "low"


async def diagnose(spot: float, strike: float, maturity: float, smile_points: int, k_steps: int, t_steps: int,
                   lower_q: float, upper_q: float) -> tuple[dict, dict]:
    """The diagnostics' summary and details."""
    surface = (await fetch_iv_surface(spot))["surface"]
    term = term_structure(surface, spot)
    iv, method = _interpolate(surface, spot, strike, maturity)
    smile = await fetch_iv_smile_slice(spot, maturity, strike, smile_points)
    skew, curvature = skew_and_curvature(smile["rows"])
    rows = grid(surface, spot, maturity, k_steps, t_steps)
    low, high = mark_zones(rows, lower_q, upper_q)
    zones = [{k: r[k] for k in ("k_ratio", "years", "iv", "zone")} for r in rows if r["zone"] != "normal"]
    score, label = confidence(term, rows)
    summary = {
        "target_strike": strike,
        "target_maturity": maturity,
        "term_points": len(term),
        "smile_points": len(smile["rows"]),
        "interp_iv": round(iv, 6) if iv is not None else None,
        "interp_iv_pct": round(iv * 100, 3) if iv is not None else None,
        "interp_method": method,
        "smile_selected_expiry": smile["selected_expiry"],
        "smile_selected_years": smile["selected_years"],
        "local_skew_slope": round(skew, 8) if skew is not None else None,
        "local_curvature": round(curvature, 8) if curvature is not None else None,
        "surface_grid_points": len(rows),
        "surface_k_steps": k_steps,
        "surface_t_steps": t_steps,
        "burst_zone_points": len(zones),
        "burst_threshold_mode": "adaptive_quantile",
        "burst_low_quantile": lower_q,
        "burst_high_quantile": upper_q,
        "burst_low_threshold_iv": round(low, 6),
        "burst_high_threshold_iv": round(high, 6),
        "confidence_score": round(score, 4),
        "confidence_label": label,
    }
    details = {"term_structure_rows": term, "smile_rows": smile["rows"], "smiles_by_expiry": smiles_by_expiry(surface, spot),
               "surface_grid_rows": rows, "burst_zone_rows": zones}
    return summary, details
