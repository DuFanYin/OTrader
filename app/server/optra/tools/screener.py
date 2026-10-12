"""The option screener on an option chain (Deribit's, or a day the trading engine recorded or backtests): the chain
itself, its snapshots or days, and strategies built from it."""

import asyncio

from optra.schemas.tools import ChainRequest, ScreenerRequest, SnapshotsRequest
from optra.infra.numerics import heston_calibrate, pricing_batch
from optra.infra.numerics import screener as run_screener_engine
from optra.services.market_data import (
    _FB_VOL,
    SUPPORTED_CURRENCIES,
    ChainUnavailable,
    _fetch_dvol,
    chain_quality,
    chain_underlyings,
    fetch_option_chain,
    list_chain_snapshots,
    load_chain_snapshot,
    surface_rows_from_chain,
)
from optra.tools.base import Result, Tool, ToolError
from optra.tools.calibration import heston_bound_warnings

HESTON_EXPIRIES = 4                  # expiries a chain-calibrated Heston is fitted to, spread over...
HESTON_YEARS = (7 / 365, 1.0)        # ...these times to expiry


async def _load_chain(currency: str, snapshot_id: str | None, at: str | None = None) -> dict:
    if snapshot_id:
        try:
            chain = await asyncio.to_thread(load_chain_snapshot, snapshot_id, at)
        except (OSError, ValueError) as exc:
            raise ToolError(f"snapshot not found: {snapshot_id} ({exc})", 404) from exc
        if chain.get("currency") != currency:
            raise ToolError(f"snapshot {snapshot_id} is not a {currency} chain")
        return chain
    try:
        return await fetch_option_chain(currency)
    except ChainUnavailable as exc:
        raise ToolError(str(exc), 503) from exc


def _source_block(chain: dict) -> dict:
    return {k: chain[k] for k in ("currency", "source", "fetched_at", "spot", "rate", "multiplier")}


def _source_warnings(chain: dict) -> list[str]:
    if chain["source"] == "live":
        return []
    return [f"using a {chain['source']} {chain['currency']} chain from {chain['fetched_at']}, not live data"]


def _calibrate_heston(data: dict) -> tuple[dict, str]:
    """Heston fitted to the chain: the calls nearest the money with an implied vol, on up to HESTON_EXPIRIES expiries,
    priced from their own vols (so the model sees the smile, as the marks do)."""
    rows = [r for r in surface_rows_from_chain(data["chain"], data["spot"]) if HESTON_YEARS[0] <= r["years"] <= HESTON_YEARS[1]]
    expiries = sorted({r["years"] for r in rows})
    chosen = set(expiries[:: max(1, len(expiries) // HESTON_EXPIRIES)][:HESTON_EXPIRIES])
    rows = [r for r in rows if r["years"] in chosen]
    if len(rows) < 6:
        raise ToolError("too few quotes with an implied vol to calibrate Heston on this chain; give heston explicitly", 422)
    spot = data["spot"]
    prices = [p["bs"] for p in pricing_batch([{"spot": spot, "strike": r["strike"], "rate": 0.0, "vol": r["iv"],
                                               "maturity": r["years"], "n_paths": 100} for r in rows])]
    atm = min(rows, key=lambda r: abs(r["strike"] - spot))["iv"]
    fit = heston_calibrate(spot=spot, rate=0.0, market_strikes=[r["strike"] for r in rows],
                           market_maturities=[r["years"] for r in rows], market_prices=prices,
                           init_v0=atm * atm, init_theta=atm * atm)
    rel = fit["rmse"] / (sum(prices) / len(prices))
    params = {k: fit[k] for k in ("v0", "kappa", "theta", "xi", "rho")}
    return {**params, "rmse_rel": rel, "quotes": len(rows), "expiries": len(chosen)}, \
        f"model vol: Heston calibrated to {len(rows)} quotes on {len(chosen)} expiries (RMSE {rel:.2%} of the average quote)"


async def screen_strategies(req: ScreenerRequest) -> Result:
    if not any(req.strategies.model_dump().values()):
        raise ToolError("select at least one strategy type")
    data = await _load_chain(req.currency, req.snapshot_id, req.at)
    quality, warnings = chain_quality(data["chain"])
    warnings = _source_warnings(data) + warnings
    notes = []

    model_vol, extra, calibrated = req.model_vol, {}, None
    if model_vol == "dvol" and req.currency not in SUPPORTED_CURRENCIES:
        raise ToolError(f"DVOL is Deribit's index: there is none for {req.currency}; use mark, surface or heston", 422)
    if model_vol == "dvol":
        dvol = await _fetch_dvol(req.currency)
        model_vol, extra = "flat", {"model_vol_flat": dvol}
        if dvol == _FB_VOL:
            warnings.append(f"model vol: {req.currency} DVOL unavailable, using the fallback {dvol:.2f}")
        else:
            notes.append(f"model vol: {req.currency} DVOL {dvol:.4f}")
    elif model_vol == "flat":
        if req.model_vol_flat is None:
            raise ToolError("model_vol_flat is required when model_vol is 'flat'", 422)
        extra = {"model_vol_flat": req.model_vol_flat}
    elif model_vol == "surface":
        rows = surface_rows_from_chain(data["chain"], data["spot"])
        if not rows:
            raise ToolError("the chain has no valid IVs to build a surface", 422)
        extra = {"surface": {"strikes": [r["strike"] for r in rows], "expiries": [r["years"] for r in rows],
                             "ivs": [r["iv"] for r in rows]}}
    elif model_vol == "heston":
        if req.heston is not None:
            extra = {"heston": req.heston.model_dump()}
        else:
            calibrated, note = await asyncio.to_thread(_calibrate_heston, data)
            extra = {"heston": {k: calibrated[k] for k in ("v0", "kappa", "theta", "xi", "rho")}}
            notes.append(note)
            warnings += heston_bound_warnings(calibrated)

    rate = data["rate"] if req.rate is None else req.rate
    engine_request = {
        "spot": data["spot"], "rate": rate, "multiplier": data["multiplier"],
        "price_mode": req.price_mode, "model_vol": model_vol, **extra,
        "chain": data["chain"],
        "strategies": req.strategies.model_dump(),
        "option_filter": req.option_filter.model_dump(),
        "strategy_filter": req.strategy_filter.model_dump(),
        "rank": req.rank.model_dump(exclude_none=True),
    }
    r = await asyncio.to_thread(run_screener_engine, engine_request)
    if r.get("status") != "ok":
        err = r.get("error") or {}
        raise ToolError(f"screener failed: {err.get('message') or err.get('code')}")
    summary = {**r["result_summary"], **_source_block(data), "rate": rate, "model_vol_requested": req.model_vol,
               "chain_quality": quality}
    if calibrated:
        summary["heston_calibrated"] = calibrated
    return Result(summary=summary,
                  details={"strategies": r["result_details"]["strategies"], "expiries": data["expiries"]},
                  notes=notes, warnings=warnings)


async def option_chain(req: ChainRequest) -> Result:
    data = await _load_chain(req.currency, req.snapshot_id, req.at)
    rows, expiries = data["chain"], data["expiries"]
    if req.max_days is not None:
        rows = [r for r in rows if r["years"] * 365.0 <= req.max_days]
        expiries = [e for e in expiries if e["years"] * 365.0 <= req.max_days]
    quality, warnings = chain_quality(rows)
    return Result(summary={**_source_block(data), "n_options": len(rows), "n_expiries": len(expiries), "chain_quality": quality},
                  details={"expiries": expiries, "chain": rows}, warnings=_source_warnings(data) + warnings)


def chain_snapshots(req: SnapshotsRequest) -> Result:
    snaps = list_chain_snapshots(req.currency)
    return Result(summary={"snapshots": [{k: s[k] for k in ("snapshot_id", "currency", "day") if k in s} for s in snaps],
                           "underlyings": chain_underlyings()})


TOOLS = [
    Tool("option_chain", "Deribit option chain",
         "The live BTC or ETH option chain from Deribit, normalized: per option the strike, expiry, years to expiry, the "
         "expiry's forward, bid, ask and mark in USD per coin, mark implied vol, volume and open interest; per expiry the "
         "forward and implied carry; and chain_quality (two-sided share, median spread, stale marks, missing IVs). Falls back "
         "to the newest cached snapshot offline (result_summary.source says which). Where the server has the trading "
         "engine's days, also SPXW or SPY at a time of a recorded or backtest day (snapshot_id and at), USD per share, "
         "implied vols from the engine's own numerics. Use max_days to keep it small.",
         ChainRequest, option_chain, "/lab/screener/chain", method="GET", engine=False, network=True,
         example={"currency": "BTC", "max_days": 30}),
    Tool("chain_snapshots", "Cached chain snapshots",
         "Lists the cached Deribit chains (the last 20 per currency) that option_chain and screen_strategies can use instead "
         "of the live chain, by snapshot_id, newest first; for SPXW or SPY, the recorded and backtest days (with at, a "
         "time on that day).",
         SnapshotsRequest, chain_snapshots, "/lab/screener/snapshots", method="GET", engine=False, example={"currency": "BTC"}),
    Tool("screen_strategies", "Screen option strategies",
         "Builds strategies from the live Deribit BTC or ETH chain (single calls, iron condors, straddles, strangles, "
         "calendars), filters the options and the strategies, prices them at executable (or mid) prices and ranks them, e.g. "
         "by edge = model value − cost against a model vol (Deribit marks, DVOL, a flat vol, a surface, or Heston, "
         "calibrated to the chain when not given). Use it to find trades with given properties. Returns result_details."
         "strategies (legs with strikes, expiries, fills and Greeks; cost, max gain and loss, reward/risk, net Greeks, edge) "
         "and in result_summary the funnel counts and chain_quality. Any strategy's legs can go to price_strategy or "
         "stress_test.",
         ScreenerRequest, screen_strategies, "/lab/screener/run", network=True,
         example={"currency": "BTC", "strategies": {"strangles": True}, "option_filter": {"days_to_expiry_range": [7, 45]},
                  "rank": {"key": "edge", "top_n": 5}}),
]
