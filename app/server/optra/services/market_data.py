"""
market_data.py — live crypto market data fetcher (BTC, plus ETH for spot / DVOL / options).

Sources
-------
1. Spot (BTC, ETH)       — Binance REST
2. DVOL index            — Deribit REST  (index-level scalar vol)
3. Option chain + IV surface — Deribit REST (full chain in two bulk requests)
4. BTC perpetual funding — Binance Futures REST  (→ real-world drift mu)
5. USD rate term structure— US Treasury FiscalData REST (1M/3M/6M/1Y tenors)

All requests have short timeouts; a value that could not be fetched is a fixed fallback, and says so: each fetch
returns its value with its source ("live" or "fallback").
"""

from __future__ import annotations

import asyncio
import json
import logging
import math
import statistics
import time
import xml.etree.ElementTree as ET  # the Treasury's rate feed
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import httpx

from optra import settings
from optra.data import chains
from optra.data.chains import expiry_table

log = logging.getLogger(__name__)

_TIMEOUT, _SLOW_TIMEOUT = 6.0, 20.0   # seconds; the slow one for the government endpoint
_clients: dict[str, httpx.AsyncClient] = {}
_clients_loop: asyncio.AbstractEventLoop | None = None


def _http(slow: bool = False) -> httpx.AsyncClient:
    """The shared HTTP client, made on first use in the running event loop. A client belongs to the loop it
    was made in, so another loop (a test client, the CLI) gets its own."""
    global _clients_loop
    loop = asyncio.get_running_loop()
    if loop is not _clients_loop:
        _clients.clear()
        _clients_loop = loop
    key = "slow" if slow else "fast"
    if key not in _clients:
        _clients[key] = httpx.AsyncClient(timeout=_SLOW_TIMEOUT if slow else _TIMEOUT)
    return _clients[key]


async def close() -> None:
    """Stops a rate refresh still running and closes the clients of the running loop (the server's shutdown)."""
    if _rate_task is not None and not _rate_task.done() and _rate_task.get_loop() is asyncio.get_running_loop():
        _rate_task.cancel()
    if _clients_loop is asyncio.get_running_loop():
        for client in _clients.values():
            await client.aclose()
    _clients.clear()

# ── Fallback constants ────────────────────────────────────────────────────────
_FB_SPOT    = 65_000.0
_FB_VOL     = 0.80
_FB_SPOT_BY_CCY = {"BTC": _FB_SPOT, "ETH": 2_500.0}
_FB_RATE    = 0.045
_FB_FUNDING = 0.0001   # ~10 bps per 8 h — typical neutral funding


# ── Helpers ───────────────────────────────────────────────────────────────────

def _years_to_expiry(expiry_ts_ms: int) -> float:
    now = datetime.now(timezone.utc).timestamp()
    return max((expiry_ts_ms / 1000 - now) / (365.25 * 24 * 3600), 1e-6)


# ── 1. Spot ───────────────────────────────────────────────────────────────────

async def _fetch_spot(currency: str = "BTC") -> tuple[float, str]:
    url = "https://api.binance.com/api/v3/ticker/price"
    fallback = _FB_SPOT_BY_CCY.get(currency, _FB_SPOT)
    try:
        r = await _http().get(url, params={"symbol": f"{currency}USDT"})
        r.raise_for_status()
        return float(r.json()["price"]), "live"
    except Exception as exc:
        log.warning("%s spot failed (%s), fallback %.0f", currency, exc, fallback)
        return fallback, "fallback"


# ── 2. DVOL index ─────────────────────────────────────────────────────────────

async def _fetch_dvol(currency: str = "BTC") -> tuple[float, str]:
    """Deribit DVOL — crypto VIX equivalent (BTC or ETH). Returns annualised decimal."""
    url = "https://www.deribit.com/api/v2/public/get_volatility_index_data"
    now_ms   = int(datetime.now(timezone.utc).timestamp() * 1000)
    start_ms = now_ms - 2 * 3_600_000
    params   = {"currency": currency, "resolution": "3600",
                "start_timestamp": start_ms, "end_timestamp": now_ms}
    try:
        r = await _http().get(url, params=params)
        r.raise_for_status()
        candles = r.json().get("result", {}).get("data", [])
        if not candles:
            raise ValueError("empty DVOL candles")
        return float(candles[-1][4]) / 100.0, "live"
    except Exception as exc:
        log.warning("DVOL failed (%s), fallback %.2f", exc, _FB_VOL)
        return _FB_VOL, "fallback"


# ── 3. Option chain (Deribit) and IV surface ─────────────────────────────────
#
# The full chain comes from two bulk public endpoints joined on instrument_name:
#   get_instruments                -> strike, option_type, expiration_timestamp
#   get_book_summary_by_currency   -> bid / ask / mark (in coin), mark_iv, volume,
#                                     open_interest, underlying_price (per-expiry forward)
# Deribit quotes option prices in the base coin; USD = coin price × underlying_price
# (the expiry's forward), which matches Deribit's own Black-76 marks. Deribit prices
# with interest_rate = 0, which is carried through as the chain's default rate.

SUPPORTED_CURRENCIES = ("BTC", "ETH")

_CHAIN_TTL      = 60.0                 # seconds; crypto trades 24/7
_CHAIN_KEEP     = 20                   # on-disk snapshots kept per currency
_IV_VALID_RANGE = (0.05, 5.0)          # mark_iv outside this is treated as unknown
_FIXTURE_DIR    = Path(__file__).resolve().parent / "fixtures"


def _chain_cache_dir() -> Path:
    return settings.home() / "chains"


_CHAIN_MEMO: dict[str, tuple[float, dict]] = {}   # currency -> (monotonic time, chain)


class ChainUnavailable(RuntimeError):
    """No live chain, no cached snapshot and no fixture for the currency."""


def _engine_underlyings() -> list[str]:
    """The trading engine's underlyings with backtest or recorded days (data/chains)."""
    return chains.symbols()


def chain_underlyings() -> list[str]:
    """What a chain can be asked for: Deribit's currencies, then the trading engine's underlyings."""
    return [*SUPPORTED_CURRENCIES, *_engine_underlyings()]


def _check_currency(currency: str) -> str:
    cur = currency.upper()
    if cur not in SUPPORTED_CURRENCIES and cur not in _engine_underlyings():
        raise ValueError(f"unsupported underlying: {currency}")
    return cur


async def _fetch_options_instruments(currency: str = "BTC") -> list[dict]:
    """Return all live option instruments for the currency from Deribit."""
    url = "https://www.deribit.com/api/v2/public/get_instruments"
    params = {"currency": currency, "kind": "option", "expired": "false"}
    r = await _http().get(url, params=params)
    r.raise_for_status()
    return r.json().get("result", [])


async def _fetch_book_summaries(currency: str = "BTC") -> list[dict]:
    """Return the book summary (quotes, mark, IV, volume, OI) of every option in one call."""
    url = "https://www.deribit.com/api/v2/public/get_book_summary_by_currency"
    r = await _http().get(url, params={"currency": currency, "kind": "option"})
    r.raise_for_status()
    return r.json().get("result", [])


def _positive(v: Any) -> float | None:
    return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) and v > 0 else None


def normalize_deribit_chain(instruments: list[dict], summaries: list[dict],
                            now_ms: float | None = None) -> list[dict]:
    """
    Join Deribit instruments and book summaries into screener chain rows
    (the ChainOption shape of the screener task). All prices are USD per 1 coin.

    - bid / ask are None when that side has no quote (Deribit sends null or 0)
    - iv is 0.0 when mark_iv is missing or outside _IV_VALID_RANGE
    - volume / oi are in coins, not contracts
    - expired instruments and rows without an underlying price are dropped
    - prices convert at each row's own underlying_price (what Deribit marked it with),
      but `forward` is the per-expiry median: the bulk endpoint is not an atomic
      snapshot, so rows of one expiry carry slightly different underlying prices
    """
    if now_ms is None:
        now_ms = datetime.now(timezone.utc).timestamp() * 1000
    by_name = {i["instrument_name"]: i for i in instruments if i.get("instrument_name")}
    rows: list[dict] = []
    for s in summaries:
        name = s.get("instrument_name", "")
        inst = by_name.get(name)
        forward = _positive(s.get("underlying_price"))
        if inst is None or forward is None:
            continue
        expiry_ms = inst.get("expiration_timestamp") or 0
        if expiry_ms <= now_ms:
            continue
        bid, ask = _positive(s.get("bid_price")), _positive(s.get("ask_price"))
        mark = _positive(s.get("mark_price")) or 0.0
        iv_pct = s.get("mark_iv")
        iv = float(iv_pct) / 100.0 if isinstance(iv_pct, (int, float)) else 0.0
        if not (_IV_VALID_RANGE[0] < iv < _IV_VALID_RANGE[1]):
            iv = 0.0
        rows.append({
            "symbol":      name,
            "option_type": inst.get("option_type", ""),
            "expiry":      name.split("-")[1] if name.count("-") >= 3 else "",
            "expiry_ts":   expiry_ms,
            "strike":      float(inst["strike"]),
            "forward":     forward,
            "years":       max((expiry_ms - now_ms) / 1000 / (365.25 * 24 * 3600), 1e-6),
            "bid":         bid * forward if bid is not None else None,
            "ask":         ask * forward if ask is not None else None,
            "mark":        mark * forward,
            "iv":          iv,
            "volume":      float(s.get("volume") or 0.0),
            "oi":          float(s.get("open_interest") or 0.0),
        })
    by_expiry: dict[str, list[float]] = {}
    for r in rows:
        by_expiry.setdefault(r["expiry"], []).append(r["forward"])
    medians = {e: statistics.median(v) for e, v in by_expiry.items()}
    for r in rows:
        r["forward"] = medians[r["expiry"]]
    rows.sort(key=lambda r: (r["expiry_ts"], r["strike"], r["option_type"]))
    return rows


# What makes a chain doubtful: thresholds of chain_quality's warnings
_TWO_SIDED_MIN, _SPREAD_MAX, _MARKS_OUTSIDE_MAX, _IV_MISSING_MAX = 0.5, 0.10, 0.05, 0.2


def chain_quality(chain: list[dict]) -> tuple[dict, list[str]]:
    """Checks on a normalized chain's quotes, and warnings for what fails: few two-sided quotes, wide spreads,
    marks outside their own quotes (stale), crossed quotes, options without a usable implied vol."""
    n = len(chain)
    two_sided = [r for r in chain if r["bid"] is not None and r["ask"] is not None]
    crossed = [r for r in two_sided if r["bid"] > r["ask"]]
    spreads = sorted((r["ask"] - r["bid"]) / ((r["ask"] + r["bid"]) / 2) for r in two_sided if r["ask"] + r["bid"] > 0)
    outside = [r for r in two_sided if r["mark"] < r["bid"] * (1 - 1e-9) or r["mark"] > r["ask"] * (1 + 1e-9)]
    no_iv = [r for r in chain if r["iv"] <= 0]
    quality = {
        "options": n,
        "two_sided_share": len(two_sided) / n if n else 0.0,
        "median_spread_pct": statistics.median(spreads) if spreads else None,
        "marks_outside_quotes": len(outside),
        "crossed_quotes": len(crossed),
        "iv_missing_share": len(no_iv) / n if n else 0.0,
    }
    warnings = []
    if n == 0:
        warnings.append("the chain is empty")
    if n and quality["two_sided_share"] < _TWO_SIDED_MIN:
        warnings.append(f"only {quality['two_sided_share']:.0%} of the options are quoted on both sides")
    if spreads and quality["median_spread_pct"] > _SPREAD_MAX:
        warnings.append(f"wide quotes: the median bid-ask spread is {quality['median_spread_pct']:.0%} of the mid")
    if two_sided and len(outside) / len(two_sided) > _MARKS_OUTSIDE_MAX:
        warnings.append(f"{len(outside)} marks lie outside their own bid and ask (stale marks)")
    if crossed:
        warnings.append(f"{len(crossed)} crossed quotes (bid above ask)")
    if n and quality["iv_missing_share"] > _IV_MISSING_MAX:
        warnings.append(f"{quality['iv_missing_share']:.0%} of the options have no usable implied vol")
    return quality, warnings


def _build_chain(currency: str, instruments: list[dict], summaries: list[dict],
                 fetched_at: datetime, source: str) -> dict:
    now_ms = fetched_at.timestamp() * 1000
    chain = normalize_deribit_chain(instruments, summaries, now_ms)
    # Deribit index price; every summary row carries the same value
    spot = next((float(s["estimated_delivery_price"]) for s in summaries
                 if _positive(s.get("estimated_delivery_price"))), 0.0)
    rate = next((float(s["interest_rate"]) for s in summaries
                 if isinstance(s.get("interest_rate"), (int, float))), 0.0)
    return {
        "currency":      currency,
        "source":        source,             # live | cache | fixture
        "fetched_at":    fetched_at.isoformat(),
        "spot":          spot,
        "rate":          rate,               # Deribit pricing rate (0 in practice)
        "multiplier":    1.0,                # 1 contract = 1 coin
        "n_instruments": len(instruments),
        "expiries":      expiry_table(chain, spot),
        "chain":         chain,
    }


def _write_snapshot(chain: dict) -> None:
    try:
        d = _chain_cache_dir()
        d.mkdir(parents=True, exist_ok=True)
        stamp = datetime.fromisoformat(chain["fetched_at"]).strftime("%Y%m%d_%H%M%S")
        path = d / f"{chain['currency']}_{stamp}.json"
        tmp = path.with_suffix(".tmp")
        tmp.write_text(json.dumps(chain, separators=(",", ":")))
        tmp.replace(path)
        for old in sorted(d.glob(f"{chain['currency']}_*.json"))[:-_CHAIN_KEEP]:
            old.unlink(missing_ok=True)
    except OSError as exc:
        log.warning("Chain snapshot write failed (%s)", exc)


def list_chain_snapshots(currency: str | None = None) -> list[dict]:
    """On-disk snapshots, newest first: [{"snapshot_id", "currency", "path"}]. The trading engine's
    underlying's are its days, newest first: [{"snapshot_id": "<UNDERLYING>_<YYYYMMDD>", "currency", "day"}]."""
    if currency and currency.upper() in _engine_underlyings():
        cur = currency.upper()
        return [{"snapshot_id": f"{cur}_{day.replace('-', '')}", "currency": cur, "day": day}
                for day in sorted(chains.days(cur), reverse=True)]
    d = _chain_cache_dir()
    if not d.exists():
        return []
    pattern = f"{_check_currency(currency)}_*.json" if currency else "*_*.json"
    return [{"snapshot_id": p.stem, "currency": p.stem.split("_")[0], "path": str(p)}
            for p in sorted(d.glob(pattern), reverse=True)]


def load_chain_snapshot(snapshot_id: str, at: str | None = None) -> dict:
    """Load a cached snapshot by id (file stem), as returned by list_chain_snapshots(); the trading
    engine's day at `at` (None: its last quotes)."""
    if "/" in snapshot_id or "\\" in snapshot_id or snapshot_id.startswith("."):
        raise ValueError("bad snapshot id")
    underlying, _, day = snapshot_id.partition("_")
    if underlying in _engine_underlyings():
        if len(day) != 8 or not day.isdigit():
            raise ValueError("bad snapshot id")
        return chains.load(underlying, f"{day[:4]}-{day[4:6]}-{day[6:]}", at)
    if at is not None:
        raise ValueError("a time is only for a recorded day's chain")
    path = _chain_cache_dir() / f"{snapshot_id}.json"
    chain = json.loads(path.read_text())
    chain["source"] = "cache"
    return chain


def load_chain_fixture(currency: str = "BTC") -> dict:
    """Recorded raw Deribit responses (services/fixtures/deribit_<cur>_chain.json), normalized."""
    cur = _check_currency(currency)
    raw = json.loads((_FIXTURE_DIR / f"deribit_{cur.lower()}_chain.json").read_text())
    fetched_at = datetime.fromisoformat(raw["fetched_at"])
    return _build_chain(cur, raw["instruments"], raw["summaries"], fetched_at, "fixture")


async def _chain_from_engine(cur: str) -> dict:
    """The trading engine's underlying: its newest day's last quotes."""
    return await asyncio.to_thread(chains.load, cur, None, None)


async def _chain_from_memo(cur: str) -> dict | None:
    memo = _CHAIN_MEMO.get(cur)
    return memo[1] if memo is not None and time.monotonic() - memo[0] < _CHAIN_TTL else None


async def _chain_from_deribit(cur: str) -> dict:
    instruments, summaries = await asyncio.gather(_fetch_options_instruments(cur), _fetch_book_summaries(cur))
    if not summaries:
        raise ValueError("empty book summary")
    chain = _build_chain(cur, instruments, summaries, datetime.now(timezone.utc), "live")
    _CHAIN_MEMO[cur] = (time.monotonic(), chain)
    _write_snapshot(chain)
    return chain


async def _chain_from_snapshot(cur: str) -> dict | None:
    for snap in list_chain_snapshots(cur):
        try:
            return load_chain_snapshot(snap["snapshot_id"])
        except (OSError, ValueError) as exc:
            log.warning("Snapshot %s unreadable (%s)", snap["snapshot_id"], exc)
    return None


async def _chain_from_fixture(cur: str) -> dict:
    return load_chain_fixture(cur)


async def fetch_option_chain(currency: str = "BTC", *, allow_stale: bool = True) -> dict:
    """
    Full option chain for BTC or ETH, from the first of its sources that has one, in order: the trading engine's
    underlying from its newest day's last quotes; else the last fetch from Deribit (memoized for _CHAIN_TTL
    seconds), Deribit itself (two HTTP requests, each fetch written to ~/.optra/chains/, override with OPTRA_HOME),
    and, with allow_stale, the newest snapshot on disk, then the bundled fixture. `source` and `fetched_at` say which.
    """
    cur = _check_currency(currency)
    if cur in _engine_underlyings():
        sources = [("engine", _chain_from_engine)]
    else:
        sources = [("memo", _chain_from_memo), ("live", _chain_from_deribit)]
        if allow_stale:
            sources += [("cache", _chain_from_snapshot), ("fixture", _chain_from_fixture)]
    failed = []
    for name, source in sources:
        try:
            chain = await source(cur)
        except Exception as exc:
            log.warning("%s option chain from %s failed (%s)", cur, name, exc)
            failed.append(f"{name}: {exc}")
            continue
        if chain is not None:
            return chain
    raise ChainUnavailable(f"{cur} option chain unavailable ({'; '.join(failed) or 'no source has it'})")


def surface_rows_from_chain(chain: list[dict], spot: float, strikes_per_expiry: int = 7) -> list[dict]:
    """Calls with a valid IV, the `strikes_per_expiry` strikes nearest spot per expiry, in expiry order."""
    calls_by_expiry: dict[str, list[dict]] = {}
    for row in chain:
        if row["option_type"] == "call" and row["iv"] > 0:
            calls_by_expiry.setdefault(row["expiry"], []).append(row)
    surface = []
    for rows in calls_by_expiry.values():          # chain is already in expiry order
        chosen = sorted(rows, key=lambda r: abs(r["strike"] - spot))[:strikes_per_expiry]
        for row in chosen:
            surface.append({
                "expiry":     row["expiry"],
                "instrument": row["symbol"],
                "strike":     row["strike"],
                "years":      round(row["years"], 4),
                "iv":         round(row["iv"], 4),
            })
    return surface


async def fetch_iv_surface(spot: float, strikes_per_expiry: int = 7,
                           currency: str = "BTC") -> dict[str, Any]:
    """
    Build an IV surface with multiple strikes per expiry so the C++ engine
    can bilinearly interpolate in both K and T dimensions. Built from the
    live chain only (a stale snapshot could sit at a different price level).

    Returns
    -------
    {
      "surface": [
        {"expiry": "27JUN25", "instrument": "BTC-27JUN25-65000-C", "strike": 65000,
         "years": 0.19, "iv": 0.83},
        ...  # multiple rows per expiry, spread around ATM, expiries in time order
      ],
      "raw_instruments_count": 412,
    }
    """
    try:
        data = await fetch_option_chain(currency, allow_stale=False)
    except Exception as exc:
        log.warning("Options chain failed for IV surface (%s)", exc)
        return {"surface": [], "raw_instruments_count": 0}

    surface = surface_rows_from_chain(data["chain"], spot, strikes_per_expiry)
    return {"surface": surface, "raw_instruments_count": data["n_instruments"]}


async def fetch_iv_smile_slice(spot: float, target_years: float, center_strike: float,
                               n_points: int = 11, currency: str = "BTC") -> dict[str, Any]:
    """
    Build a same-expiry strike slice (smile proxy) near target maturity.
    Returns rows sorted by strike with mark IV values.
    """
    empty = {"rows": [], "selected_expiry": "", "selected_years": 0.0}
    try:
        data = await fetch_option_chain(currency, allow_stale=False)
    except Exception as exc:
        log.warning("Options chain failed for smile slice (%s)", exc)
        return empty

    calls_by_expiry: dict[str, list[dict]] = {}
    for row in data["chain"]:
        if row["option_type"] == "call":
            calls_by_expiry.setdefault(row["expiry"], []).append(row)
    if not calls_by_expiry:
        return empty

    selected_expiry = min(calls_by_expiry, key=lambda e: abs(calls_by_expiry[e][0]["years"] - target_years))
    calls = calls_by_expiry[selected_expiry]
    selected_years = float(calls[0]["years"])
    pick_n = max(5, min(n_points, len(calls)))
    chosen = sorted(sorted(calls, key=lambda r: abs(r["strike"] - center_strike))[:pick_n],
                    key=lambda r: r["strike"])

    rows = [
        {
            "instrument": r["symbol"],
            "strike": r["strike"],
            "years": round(selected_years, 4),
            "log_moneyness": round(math.log(max(r["strike"], 1e-8) / max(spot, 1e-8)), 6),
            "iv": round(r["iv"], 6),
        }
        for r in chosen if r["iv"] > 0
    ]
    return {"rows": rows, "selected_expiry": selected_expiry, "selected_years": round(selected_years, 4)}


# ── 4. Funding rate → mu ──────────────────────────────────────────────────────

async def _fetch_funding_rate() -> tuple[float, str]:
    """
    Binance perpetual funding rate for BTCUSDT.
    Funding is paid every 8 h, so annualised mu ≈ spot_rate * 3 * 365.
    Returns annualised decimal under the physical measure.
    """
    url = "https://fapi.binance.com/fapi/v1/premiumIndex"
    try:
        r = await _http().get(url, params={"symbol": "BTCUSDT"})
        r.raise_for_status()
        rate_str = r.json().get("lastFundingRate", "")
        if not rate_str:
            raise ValueError("missing lastFundingRate")
        rate_8h = float(rate_str)
        # annualise: 3 payments/day × 365 days
        return rate_8h * 3 * 365, "live"
    except Exception as exc:
        log.warning("Funding rate failed (%s), fallback %.4f", exc, _FB_FUNDING)
        return _FB_FUNDING, "fallback"


# ── 5. Rate term structure ────────────────────────────────────────────────────

_TENOR_YEARS = {"1m": 1/12, "3m": 0.25, "6m": 0.5, "1y": 1.0, "2y": 2.0}

_FALLBACK_CURVE = {"1m": 0.043, "3m": 0.045, "6m": 0.046, "1y": 0.047, "2y": 0.047}

# The Treasury site takes about 18 s to answer (its own processing; the connection is quick), and its data changes once
# a day. So a curve is kept in memory and on disk ($OPTRA_HOME/rates.json), refreshed in the background, and a caller
# waits for the site only when there is no curve of the last day at all.
_RATE_FRESH = 3600.0          # seconds a curve is used as it is
_RATE_STALE = 24 * 3600.0     # seconds an older curve is still served while a refresh runs
_RATE_WAIT = 25.0             # seconds a caller waits for the site when no curve is young enough


@dataclass
class _KeptCurve:
    curve: dict[str, float] | None = None
    as_of: str | None = None     # the curve's date
    fetched: float = 0.0         # when it was fetched, wall-clock seconds (so a saved one keeps its age)
    source: str | None = None    # treasury (fetched by this process) or saved (on disk, from an earlier one)

    def age(self) -> float:
        return time.time() - self.fetched if self.curve is not None else math.inf


_rate = _KeptCurve()
_rate_task: asyncio.Task | None = None


def _rates_file() -> Path:
    return settings.home() / "rates.json"


async def _download_rate_curve() -> tuple[dict[str, float], str | None]:
    """The latest daily curve from the Treasury's XML feed, and its date."""
    month = datetime.now(timezone.utc).strftime("%Y%m")
    url = ("https://home.treasury.gov/resource-center/data-chart-center/interest-rates/pages/xml"
           f"?data=daily_treasury_yield_curve&field_tdr_date_value_month={month}")
    ns, atom = "http://schemas.microsoft.com/ado/2007/08/dataservices", "http://www.w3.org/2005/Atom"
    fields = {"BC_1MONTH": "1m", "BC_3MONTH": "3m", "BC_6MONTH": "6m", "BC_1YEAR": "1y", "BC_2YEAR": "2y"}
    r = await _http(slow=True).get(url)
    r.raise_for_status()
    entries = ET.fromstring(r.text).findall(f"{{{atom}}}entry")
    if not entries:
        raise ValueError("no XML entries")
    # the last entry is the latest date; two-step find avoids path namespace issues
    content = entries[-1].find(f"{{{atom}}}content")
    props = content.find(f"{{{ns}/metadata}}properties") if content is not None else None
    if props is None:
        raise ValueError("no properties element")
    curve = {tenor: round(float(el.text) / 100.0, 6) for field, tenor in fields.items()
             if (el := props.find(f"{{{ns}}}{field}")) is not None and el.text}
    if not curve:
        raise ValueError("no rates in the latest entry")
    date = props.find(f"{{{ns}}}NEW_DATE")
    return {**_FALLBACK_CURVE, **curve}, (date.text[:10] if date is not None and date.text else None)


async def refresh_rate_curve() -> None:
    """Fetches the curve and keeps it (in memory and on disk); on failure keeps what there was."""
    try:
        curve, as_of = await _download_rate_curve()
    except Exception as exc:
        log.warning("Rate curve failed (%s)", exc)
        return
    _rate.curve, _rate.as_of, _rate.fetched, _rate.source = curve, as_of, time.time(), "treasury"
    try:
        path = _rates_file()
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(".tmp")
        tmp.write_text(json.dumps({"curve": curve, "as_of": as_of, "fetched": _rate.fetched}))
        tmp.replace(path)
    except OSError as exc:
        log.warning("Rate curve write failed (%s)", exc)


def _load_saved_curve() -> None:
    """The curve an earlier fetch saved, when there is none in memory."""
    if _rate.curve is not None:
        return
    try:
        saved = json.loads(_rates_file().read_text())
        _rate.curve, _rate.as_of, _rate.fetched, _rate.source = saved["curve"], saved.get("as_of"), float(saved["fetched"]), "saved"
    except (OSError, ValueError, KeyError, TypeError):
        pass


def _refreshing() -> asyncio.Task:
    """The refresh running in this event loop, started if there is none."""
    global _rate_task
    loop = asyncio.get_running_loop()
    if _rate_task is None or _rate_task.done() or _rate_task.get_loop() is not loop:
        _rate_task = loop.create_task(refresh_rate_curve())
    return _rate_task


def start_rate_refresh() -> None:
    """Starts fetching the curve in the background (the server does at startup) unless a fresh one is at hand."""
    _load_saved_curve()
    if _rate.age() >= _RATE_FRESH:
        _refreshing()


async def _fetch_rate_curve() -> dict[str, float]:
    """The US Treasury curve (1m to 2y, decimals): at once from memory or disk when it is less than a day old (refreshed in
    the background after an hour), else after waiting for the site up to _RATE_WAIT seconds; the fixed fallback when
    there is nothing at all. rate_curve_info() says which."""
    _load_saved_curve()
    if _rate.age() < _RATE_FRESH:
        return _rate.curve
    task = _refreshing()
    if _rate.age() >= _RATE_STALE:
        try:
            await asyncio.wait_for(asyncio.shield(task), _RATE_WAIT)
        except (TimeoutError, asyncio.TimeoutError):
            pass
    return _rate.curve if _rate.curve is not None else dict(_FALLBACK_CURVE)


def rate_curve_info() -> dict:
    """Where the curve _fetch_rate_curve returns comes from (treasury, saved, or fallback: fixed values), its date, and
    how old the fetch is."""
    if _rate.curve is None:
        return {"source": "fallback", "as_of": None, "age_hours": None}
    return {"source": _rate.source, "as_of": _rate.as_of, "age_hours": round(_rate.age() / 3600, 2)}


def pick_rate(curve: dict[str, float], target_years: float) -> tuple[str, float]:
    """
    Return the (tenor_key, rate) from curve whose maturity is nearest to
    target_years. Interpolates linearly between the two bracketing tenors.
    """
    points = sorted(
        ((y, k, curve[k]) for k, y in _TENOR_YEARS.items() if k in curve),
        key=lambda x: x[0],
    )
    if not points:
        return "3m", _FALLBACK_CURVE["3m"]

    # clamp to endpoints
    if target_years <= points[0][0]:
        return points[0][1], points[0][2]
    if target_years >= points[-1][0]:
        return points[-1][1], points[-1][2]

    # linear interpolation between bracketing points
    for i in range(len(points) - 1):
        y0, k0, r0 = points[i]
        y1, k1, r1 = points[i + 1]
        if y0 <= target_years <= y1:
            t = (target_years - y0) / (y1 - y0)
            label = f"{k0}→{k1}"
            return label, r0 + t * (r1 - r0)

    return points[-1][1], points[-1][2]


# ── Public composite fetcher ──────────────────────────────────────────────────

async def fetch_btc_snapshot() -> dict:
    """
    Fetch all five data sources concurrently.
    Any individual failure uses its fallback, named in "fallback"; the call never raises.
    """
    spot_task     = _fetch_spot("BTC")
    dvol_task     = _fetch_dvol("BTC")
    funding_task  = _fetch_funding_rate()
    curve_task    = _fetch_rate_curve()

    (spot, spot_from), (dvol, dvol_from), (funding_ann, funding_from), curve = await asyncio.gather(
        spot_task, dvol_task, funding_task, curve_task,
    )

    # Options surface needs spot first (for ATM selection), fetch after
    surface_data = await fetch_iv_surface(spot)

    # Default rate = 3-month tenor
    _, rate_3m = pick_rate(curve, 0.25)

    fell_back = [name for name, source in (("spot", spot_from), ("vol", dvol_from), ("mu", funding_from),
                                           ("rate", rate_curve_info()["source"])) if source == "fallback"]
    return {
        # ── Fields written directly into form inputs ──
        "spot":         round(spot, 2),
        "vol":          round(dvol, 4),          # DVOL index as default vol
        "rate":         round(rate_3m, 4),        # 3-month rate as default
        "mu":           round(funding_ann, 4),    # annualised funding → P-drift

        "funding_8h":   round(funding_ann / (3 * 365), 6),

        # ── Full curve (for tenor-aware rate selection in the UI) ──
        "rate_curve":   curve,
        "fallback":     fell_back,                # fixed values in place of what could not be fetched

        # ── IV surface (for strike/maturity-aware vol selection) ──
        "iv_surface":   surface_data["surface"],

        "fetched_at": datetime.now(timezone.utc).isoformat(),
        "sources": {
            "spot":    "Binance BTCUSDT",
            "vol":     "Deribit DVOL index",
            "iv":      "Deribit options mark IV (ATM per expiry)",
            "mu":      "Binance perpetual funding rate (annualised)",
            "rate":    "US Treasury FiscalData",
        },
    }
