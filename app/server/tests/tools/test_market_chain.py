"""Deribit option chain (market_data.fetch_option_chain and friends).

Offline by default: Deribit is replaced by an httpx.MockTransport serving the
recorded fixtures in optra/services/fixtures/. Set OPTRA_DERIBIT_LIVE=1 to also run the
live checks against Deribit (delta vs the ticker endpoint, request count).

Run:  python tests/test_market_chain.py   (or pytest)
"""

from __future__ import annotations

import asyncio
import json
import os
import random
import sys
import tempfile
from pathlib import Path

import httpx

SERVER = Path(__file__).resolve().parents[2]  # app/server

from optra.infra import numerics  # noqa: E402

from optra.services import market_data as md  # noqa: E402

FIXTURES = SERVER / "optra" / "services" / "fixtures"


def raw_fixture(currency="BTC"):
    return json.loads((FIXTURES / f"deribit_{currency.lower()}_chain.json").read_text())


class FakeDeribit:
    """Serves fixture responses; counts requests; can be switched to fail."""

    def __init__(self, currency="BTC", fail=False):
        self.raw = raw_fixture(currency)
        self.fail = fail
        self.requests: list[str] = []

    def __call__(self, request: httpx.Request) -> httpx.Response:
        self.requests.append(request.url.path)
        if self.fail:
            raise httpx.ConnectError("offline", request=request)
        if request.url.path.endswith("get_instruments"):
            return httpx.Response(200, json={"result": self.raw["instruments"]})
        if request.url.path.endswith("get_book_summary_by_currency"):
            return httpx.Response(200, json={"result": self.raw["summaries"]})
        return httpx.Response(404)


class Env:
    """Isolated OPTRA_HOME, empty memo, mocked HTTP client."""

    def __init__(self, fake: FakeDeribit, home: str | None = None):
        self.fake, self.home = fake, home

    def __enter__(self):
        self._tmp = tempfile.TemporaryDirectory() if self.home is None else None
        self._old_home = os.environ.get("OPTRA_HOME")
        os.environ["OPTRA_HOME"] = self.home or self._tmp.name
        self._old_http = md._http
        client = httpx.AsyncClient(transport=httpx.MockTransport(self.fake))
        md._http = lambda slow=False: client
        md._CHAIN_MEMO.clear()
        return self

    def __exit__(self, *exc):
        md._http = self._old_http
        md._CHAIN_MEMO.clear()
        if self._old_home is None:
            os.environ.pop("OPTRA_HOME", None)
        else:
            os.environ["OPTRA_HOME"] = self._old_home
        if self._tmp:
            self._tmp.cleanup()


def fixture_now_ms(currency="BTC"):
    from datetime import datetime
    return datetime.fromisoformat(raw_fixture(currency)["fetched_at"]).timestamp() * 1000


# ── Normalization ─────────────────────────────────────────────────────────────

def test_normalize_converts_coin_prices_to_usd_at_forward():
    raw = raw_fixture()
    rows = md.normalize_deribit_chain(raw["instruments"], raw["summaries"], fixture_now_ms())
    by_name = {s["instrument_name"]: s for s in raw["summaries"]}
    assert len(rows) > 500
    checked = 0
    for r in rows:
        s = by_name[r["symbol"]]
        F = s["underlying_price"]                       # conversion uses the row's own price
        assert abs(r["forward"] / F - 1) < 1e-3 and r["years"] > 0
        assert abs(r["mark"] - s["mark_price"] * F) < 1e-9
        if s["bid_price"]:
            assert abs(r["bid"] - s["bid_price"] * F) < 1e-9
            checked += 1
        else:
            assert r["bid"] is None
        if r["iv"]:
            assert abs(r["iv"] - s["mark_iv"] / 100) < 1e-12
        assert r["option_type"] in ("call", "put")
        assert r["expiry"] == r["symbol"].split("-")[1]
    assert checked > 100
    keys = [(r["expiry_ts"], r["strike"]) for r in rows]
    assert keys == sorted(keys)
    forwards: dict[str, set] = {}
    for r in rows:
        forwards.setdefault(r["expiry"], set()).add(r["forward"])
    assert all(len(v) == 1 for v in forwards.values())     # one forward per expiry


def test_normalize_edge_cases():
    future = 4_102_444_800_000   # 2100-01-01
    instruments = [
        {"instrument_name": "BTC-1JAN00-100-C", "strike": 100, "option_type": "call", "expiration_timestamp": future},
        {"instrument_name": "BTC-1JAN00-200-C", "strike": 200, "option_type": "call", "expiration_timestamp": future},
        {"instrument_name": "BTC-1JAN20-100-P", "strike": 100, "option_type": "put", "expiration_timestamp": 1},
        {"instrument_name": "BTC-1JAN00-300-C", "strike": 300, "option_type": "call", "expiration_timestamp": future},
    ]
    summaries = [
        {"instrument_name": "BTC-1JAN00-100-C", "bid_price": 0, "ask_price": 0.01, "mark_price": 0.005,
         "mark_iv": 900.0, "underlying_price": 1000.0},                       # no bid, absurd iv
        {"instrument_name": "BTC-1JAN00-200-C", "bid_price": None, "ask_price": None, "mark_price": None,
         "mark_iv": None, "underlying_price": 1000.0},
        {"instrument_name": "BTC-1JAN20-100-P", "bid_price": 0.1, "ask_price": 0.2, "mark_price": 0.15,
         "mark_iv": 50, "underlying_price": 1000.0},                          # expired
        {"instrument_name": "BTC-1JAN00-300-C", "bid_price": 0.1, "ask_price": 0.2, "mark_price": 0.15,
         "mark_iv": 50, "underlying_price": None},                            # no forward
        {"instrument_name": "BTC-UNKNOWN", "bid_price": 0.1, "underlying_price": 1000.0},
    ]
    rows = md.normalize_deribit_chain(instruments, summaries, now_ms=1_000_000)
    assert [r["symbol"] for r in rows] == ["BTC-1JAN00-100-C", "BTC-1JAN00-200-C"]
    a, b = rows
    assert a["bid"] is None and abs(a["ask"] - 10.0) < 1e-12 and a["iv"] == 0.0
    assert b["bid"] is None and b["ask"] is None and b["mark"] == 0.0 and b["iv"] == 0.0


# ── Fetching, caching, fallback ───────────────────────────────────────────────

def test_fetch_option_chain_uses_two_requests_and_memoizes():
    fake = FakeDeribit()
    with Env(fake) as env:
        chain = asyncio.run(md.fetch_option_chain("btc"))
        assert len(fake.requests) == 2
        assert sorted(p.rsplit("/", 1)[1] for p in fake.requests) == [
            "get_book_summary_by_currency", "get_instruments"]
        assert chain["source"] == "live" and chain["currency"] == "BTC"
        assert chain["spot"] > 0 and chain["multiplier"] == 1.0 and chain["rate"] == 0.0
        assert chain["expiries"] and all(e["n_options"] > 0 for e in chain["expiries"])
        assert sum(e["n_options"] for e in chain["expiries"]) == len(chain["chain"])

        asyncio.run(md.fetch_option_chain("BTC"))
        assert len(fake.requests) == 2                      # memo hit within TTL
        snaps = md.list_chain_snapshots("BTC")
        assert len(snaps) == 1 and Path(env.home or os.environ["OPTRA_HOME"]).exists()


def test_fallback_order_snapshot_then_fixture():
    with tempfile.TemporaryDirectory() as home:
        with Env(FakeDeribit(), home):
            live = asyncio.run(md.fetch_option_chain("BTC"))
        with Env(FakeDeribit(fail=True), home):
            cached = asyncio.run(md.fetch_option_chain("BTC"))
            assert cached["source"] == "cache"
            assert cached["fetched_at"] == live["fetched_at"]
            assert len(cached["chain"]) == len(live["chain"])
    with Env(FakeDeribit(fail=True)):
        fx = asyncio.run(md.fetch_option_chain("BTC"))
        assert fx["source"] == "fixture" and len(fx["chain"]) > 500
        try:
            asyncio.run(md.fetch_option_chain("BTC", allow_stale=False))
            raise AssertionError("expected ChainUnavailable")
        except md.ChainUnavailable:
            pass


def test_snapshot_retention_and_ids():
    with Env(FakeDeribit()):
        chain = asyncio.run(md.fetch_option_chain("BTC"))
        for i in range(md._CHAIN_KEEP + 3):
            md._write_snapshot(dict(chain, fetched_at=f"2030-01-01T00:00:{i:02d}+00:00"))
        snaps = md.list_chain_snapshots("BTC")
        assert len(snaps) == md._CHAIN_KEEP
        assert md.load_chain_snapshot(snaps[0]["snapshot_id"])["source"] == "cache"
        for bad in ("../x", "a/b", ".hidden"):
            try:
                md.load_chain_snapshot(bad)
                raise AssertionError("expected ValueError")
            except ValueError:
                pass
        try:
            asyncio.run(md.fetch_option_chain("DOGE"))
            raise AssertionError("expected ValueError")
        except ValueError:
            pass


def test_eth_fixture_loads():
    eth = md.load_chain_fixture("ETH")
    assert eth["currency"] == "ETH" and len(eth["chain"]) > 300 and eth["spot"] > 0


# ── IV surface / smile built from the chain ───────────────────────────────────

def test_iv_surface_keeps_contract_and_needs_two_requests():
    fake = FakeDeribit()
    with Env(fake):
        spot = md.load_chain_fixture()["spot"]
        out = asyncio.run(md.fetch_iv_surface(spot))
        assert len(fake.requests) == 2
    surface = out["surface"]
    assert out["raw_instruments_count"] == len(raw_fixture()["instruments"])
    assert surface and all(set(r) == {"expiry", "instrument", "strike", "years", "iv", "iv_pct"} for r in surface)
    per_expiry: dict[str, int] = {}
    for r in surface:
        per_expiry[r["expiry"]] = per_expiry.get(r["expiry"], 0) + 1
        assert r["instrument"].endswith("-C") and r["iv"] > 0
    assert max(per_expiry.values()) <= 7
    years = [r["years"] for r in surface]
    assert years == sorted(years)


def test_iv_surface_does_not_use_stale_data():
    with Env(FakeDeribit(fail=True)):
        assert asyncio.run(md.fetch_iv_surface(80_000.0)) == {"surface": [], "raw_instruments_count": 0}
        assert asyncio.run(md.fetch_iv_smile_slice(80_000.0, 0.1, 80_000.0))["rows"] == []


def test_smile_slice_from_chain():
    with Env(FakeDeribit()):
        spot = md.load_chain_fixture()["spot"]
        out = asyncio.run(md.fetch_iv_smile_slice(spot, 0.25, spot, 11))
    strikes = [r["strike"] for r in out["rows"]]
    assert out["selected_expiry"] and 5 <= len(strikes) <= 11 and strikes == sorted(strikes)


# ── End to end: fixture chain -> C++ screener ─────────────────────────────────

def test_screener_on_deribit_fixture():
    data = md.load_chain_fixture("BTC")
    r = numerics.screener({
        "spot": data["spot"], "rate": data["rate"], "multiplier": data["multiplier"],
        "chain": data["chain"],
        "strategies": {"iron_condors": True, "strangles": True, "straddles": True},
        "option_filter": {"days_to_expiry_range": [1, 45], "max_bid_ask_spread_pct": 0.25},
        "strategy_filter": {"direction": "LONG"},
        "rank": {"key": "rr", "top_n": 30},
    })
    assert r["status"] == "ok", r.get("error")
    s = r["result_summary"]
    assert s["n_options_after_filter"] > 0 and s["n_generated"] > 0 and s["n_returned"] > 0
    forwards = {e["expiry"]: e["forward"] for e in data["expiries"]}
    for strat in r["result_details"]["strategies"]:
        for leg in strat["legs"]:
            assert leg["forward"] == forwards[leg["expiry"]]


def test_mark_model_reproduces_deribit_marks():
    """With Deribit's own rate (0) and mark_iv, Black-76 on the forward gives back the mark.

    Expiries under a week are excluded: their OTM prices move several percent on the
    seconds between Deribit computing the mark and the snapshot timestamp.
    """
    data = md.load_chain_fixture("BTC")
    chain = [o for o in data["chain"] if o["iv"] > 0 and o["mark"] > 50 and o["years"] > 7 / 365]
    r = numerics.screener({
        "spot": data["spot"], "rate": data["rate"], "chain": chain,
        "strategies": {"single_calls": True},
        "option_filter": {"require_two_sided": False}, "price_mode": "mid",
        "rank": {"key": "cost", "top_n": 500},
    })
    rows = r["result_details"]["strategies"]
    assert len(rows) > 50
    worst = max(abs(x["legs"][0]["model_price"] / x["legs"][0]["mark"] - 1) for x in rows)
    assert worst < 0.02, worst


# ── Live checks (OPTRA_DERIBIT_LIVE=1) ────────────────────────────────────────────

def test_live_delta_matches_deribit_ticker():
    if os.environ.get("OPTRA_DERIBIT_LIVE") != "1":
        print("      (skipped: set OPTRA_DERIBIT_LIVE=1)")
        return
    md._CHAIN_MEMO.clear()
    data = asyncio.run(md.fetch_option_chain("BTC", allow_stale=False))
    # Sample (expiry, strike) pairs that have both a call and a put, so straddles cover both sides.
    pairs: dict[tuple, list[dict]] = {}
    for o in data["chain"]:
        if o["iv"] > 0 and o["years"] > 2 / 365:
            pairs.setdefault((o["expiry"], o["strike"]), []).append(o)
    random.seed(7)
    keys = random.sample([k for k, v in pairs.items() if len(v) == 2], 6)
    sample = [o for k in keys for o in pairs[k]]
    r = numerics.screener({
        "spot": data["spot"], "rate": data["rate"], "chain": sample,
        "strategies": {"straddles": True}, "price_mode": "mid",
        "option_filter": {"require_two_sided": False}, "rank": {"key": "cost", "top_n": 500},
    })
    ours = {leg["symbol"]: leg["delta"] for s in r["result_details"]["strategies"] for leg in s["legs"]}
    with httpx.Client(timeout=15) as c:
        for name, delta in ours.items():
            t = c.get("https://www.deribit.com/api/v2/public/ticker", params={"instrument_name": name}).json()
            assert abs(delta - t["result"]["greeks"]["delta"]) < 0.01, (name, delta, t["result"]["greeks"]["delta"])
    assert ours


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
