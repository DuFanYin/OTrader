"""The lab's page data (/api/lab): the market data its inputs show, one source each (the market_snapshot tool has
them all), and the numerics library's tasks by name, its response unchanged. The lab's computations are tools
(api/tools.py)."""

from __future__ import annotations

import math


from fastapi import APIRouter, Depends
from pydantic import BaseModel, Field

from optra.services.market_data import (
    pick_rate,
    _fetch_spot,
    _fetch_dvol,
    _fetch_funding_rate,
    _fetch_rate_curve,
    fetch_iv_surface,
    rate_curve_info,
)
from optra.infra import access
from optra.infra.numerics import vol_surface_interp

router = APIRouter(prefix="/api/lab", tags=["lab"])
ANYONE = [Depends(access.anyone)]


# ── Sub-endpoints for progressive frontend display ────────────────────────────

@router.get("/market/spot", dependencies=ANYONE)
async def get_spot() -> dict:
    spot, source = await _fetch_spot("BTC")
    return {"spot": round(spot, 2), "source": source}


@router.get("/market/dvol", dependencies=ANYONE)
async def get_dvol() -> dict:
    dvol, source = await _fetch_dvol("BTC")
    return {"vol": round(dvol, 4), "source": source}


@router.get("/market/funding", dependencies=ANYONE)
async def get_funding() -> dict:
    funding_ann, source = await _fetch_funding_rate()
    return {"mu": round(funding_ann, 4), "funding_8h": round(funding_ann / (3 * 365), 6), "source": source}


@router.get("/market/rates", dependencies=ANYONE)
async def get_rates() -> dict:
    curve = await _fetch_rate_curve()
    _, rate_3m = pick_rate(curve, 0.25)
    return {"rate": round(rate_3m, 4), "rate_curve": curve, "source": rate_curve_info()["source"]}


@router.get("/market/surface", dependencies=ANYONE)
async def get_surface(spot: float) -> dict:
    surface_data = await fetch_iv_surface(spot)
    return {"iv_surface": surface_data["surface"]}


# ── POST /market/resolve ──────────────────────────────────────────────────────

class ResolveSurface(BaseModel):
    strike: float
    years:  float
    iv:     float
    instrument: str = ""


class ResolveRequest(BaseModel):
    target_strike:  float = Field(..., gt=0)
    target_maturity: float = Field(..., gt=0)
    spot:           float = Field(..., gt=0)
    iv_surface:     list[ResolveSurface] = []
    rate_curve:     dict[str, float] = {}


class ResolveResponse(BaseModel):
    vol:          float
    rate:         float
    rate_tenor:   str
    iv_instrument: str
    iv_matched_strike: float
    iv_matched_years:  float
    interp_method: str   # "cpp_bilinear" | "py_nearest" | "dvol_fallback"


@router.post("/market/resolve", response_model=ResolveResponse, dependencies=ANYONE)
def resolve_params(req: ResolveRequest) -> ResolveResponse:
    """
    Given a strike and maturity, resolve:
      - vol  : bilinear IV interpolation on the surface (C++ engine)
      - rate : linear interpolation on the rate term structure

    This offloads all market-parameter selection from the browser JS
    into the backend / C++ layer.
    """
    # ── Vol via C++ bilinear surface interpolation ────────────────────────────
    surface = [p.model_dump() for p in req.iv_surface]
    cpp_result = vol_surface_interp(
        surface       = surface,
        spot          = req.spot,
        target_strike = req.target_strike,
        target_expiry = req.target_maturity,
    )

    if cpp_result is not None:
        iv, interp_method = cpp_result
        # Find nearest surface point for provenance metadata
        scale = req.target_strike if req.target_strike > 0 else 1.0
        best_pt = min(
            req.iv_surface,
            key=lambda p: math.hypot(
                (p.strike - req.target_strike) / scale,
                (p.years  - req.target_maturity),
            ),
        )
        iv_instrument = best_pt.instrument
        iv_matched_k  = best_pt.strike
        iv_matched_t  = best_pt.years
    else:
        # No surface — caller should fall back to DVOL
        iv            = 0.0
        interp_method = "dvol_fallback"
        iv_instrument = ""
        iv_matched_k  = req.target_strike
        iv_matched_t  = req.target_maturity

    # ── Rate via Python linear interpolation on term structure ────────────────
    tenor_label, rate = pick_rate(req.rate_curve, req.target_maturity)

    return ResolveResponse(
        vol               = round(iv, 6),
        rate              = round(rate, 6),
        rate_tenor        = tenor_label,
        iv_instrument     = iv_instrument,
        iv_matched_strike = iv_matched_k,
        iv_matched_years  = iv_matched_t,
        interp_method     = interp_method,
    )

