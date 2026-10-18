"""Tests for the backtest days (optra.data.days, synthetic_days).

The round-trip test splits a real day from data/SPXW back into option and underlying quotes, rebuilds it with
days.convert_frame, and requires the same values (the file is written the newer, smaller way: rows by contract, the
option's prices as decimals); if the backtester is built, it also requires an identical backtest result. A day
written with prices that are not whole cents keeps them as doubles; either way it reads back as written. The example conversion (scripts/data/example_to_parquet.py) must rebuild a synthetic day from
its two CSV files, identically. The quote recorder (scripts/data/record_quotes.py, recorded_days) must write each
minute into the day file once, exactly as the engine gave the quotes, and go on with the day after a restart. Those two are skipped when the data / binary are not present. The synthetic-data tests check
synthetic_days, and, when the backtester's test_backtest_data is built, that the engine reads back the implied vol
the quotes were priced at, at the risk-free rate the day carries.
"""

from __future__ import annotations

import itertools
import json
import math
import random
import re
import statistics
import subprocess
import sys
import tempfile
from dataclasses import replace
from datetime import date, datetime
from pathlib import Path

import polars as pl
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from optra.data import days
from optra.data import synthetic_days as syn
from optra.data.days import OPTION_SCHEMA

REPO = Path(__file__).resolve().parents[4]
REAL_DAY = REPO / "data" / "SPXW" / "SPXW-2025-08" / "20250804.parquet"
BACKTEST = REPO / "engine" / "build" / "entry_backtest"
BACKTEST_DATA = (
    REPO / "engine" / "build" / "tests" / "checks" / "test_backtest_data"
)


# ── round trip on a real day ──────────────────────────────────────────────────


def _backtest(path: Path) -> dict:
    out = subprocess.run(
        [str(BACKTEST), str(path), "RandomBuyStrategy"],
        capture_output=True,
        text=True,
        check=True,
    )
    lines = [json.loads(line) for line in out.stdout.splitlines() if line.startswith("{")]
    trial, days = lines[0]["days"], lines[-1]["days"]
    return {"quotes": days["quotes"], "bars": days["bars"]} | {
        k: trial[k] for k in ("orders", "pnl", "net_pnl", "max_delta")
    }


def test_real_day_round_trip():
    if not REAL_DAY.exists():
        print("      (skipped: no data/SPXW/SPXW-2025-08/20250804.parquet)")
        return
    original = days.read_option_day(REAL_DAY)  # its prices as the engine reads them
    option_part = original.select("symbol", "ts_recv", "bid_px", "ask_px", "bid_sz", "ask_sz",
                                  *[c for c in ("open_interest",) if c in original.columns])
    und = (
        original.select("ts_recv", *[c for c in original.columns if c.startswith("underlying_")])
        .unique(subset=["ts_recv"])
        .rename(lambda c: c.removeprefix("underlying_"))
    )
    with tempfile.TemporaryDirectory() as tmp:
        curve = (pq.read_schema(REAL_DAY).metadata or {}).get(days.RISK_FREE_CURVE)  # priced with the same rates
        written = days.convert_frame(
            option_part, days.load_underlying_frame(und), Path(tmp), "SPXW", overwrite=False,
            rates=json.loads(curve) if curve else None,
        )
        assert [p.name for p in written] == ["20250804.parquet"]
        assert pa.types.is_decimal(pq.read_schema(written[0]).field("bid_px").type)
        rebuilt = days.read_option_day(written[0])
        assert rebuilt.schema == original.schema
        by_contract = original.sort("symbol", "ts_recv", maintain_order=True)
        assert rebuilt.equals(by_contract), "rebuilt day differs from data/SPXW"
        if BACKTEST.exists():
            assert _backtest(written[0]) == _backtest(REAL_DAY)
        else:
            print("      (backtest comparison skipped: engine/build/entry_backtest not built)")


# ── synthetic days ────────────────────────────────────────────────────────────

SMALL = syn.Model(strikes=2, expiries=2, seed=7)
FRIDAY, MONDAY = date(2025, 3, 7), date(2025, 3, 10)


def test_synthetic_days_have_the_backtest_layout_and_schema():
    with tempfile.TemporaryDirectory() as tmp:
        written = syn.generate(SMALL, FRIDAY, MONDAY, Path(tmp), "SPY", overwrite=False)
        rel = [str(p.relative_to(tmp)) for p in written]
        assert rel == ["SPY/SPY-2025-03/20250307.parquet", "SPY/SPY-2025-03/20250310.parquet"]
        day = days.read_option_day(written[0])
        assert dict(day.schema) == OPTION_SCHEMA
        assert day["ts_recv"].n_unique() == 391  # 09:30 to 16:00, both included
        assert day["symbol"].n_unique() == 2 * 5 * 2  # expiries x strikes x call/put
        assert sorted({s[:6] for s in day["symbol"]}) == ["250307", "250310"]  # 0DTE, Monday
        assert (Path(tmp) / "SPY" / "synthetic-20250307-20250310.json").exists()
        try:
            syn.generate(SMALL, FRIDAY, FRIDAY, Path(tmp), "SPY", overwrite=False)
            raise AssertionError("expected ConversionError")
        except days.ConversionError as exc:
            assert "already exists" in str(exc)


def test_a_day_whose_prices_are_not_whole_cents_keeps_them_as_doubles(tmp_path):
    rows = pl.DataFrame({"symbol": ["250307C00500000", "250307C00500000"], "ts_recv": [datetime(2025, 3, 7, 9, 30, tzinfo=days.ET), datetime(2025, 3, 7, 9, 31, tzinfo=days.ET)],
                         "bid_px": [1.25, 1.0 / 3], "ask_px": [1.3, None], "bid_sz": [1, 2], "ask_sz": [3, 4],
                         "underlying_bid_px": [500.0, 500.1], "underlying_ask_px": [500.02, 500.12],
                         "underlying_bid_sz": [10, 10], "underlying_ask_sz": [10, 10]})
    day = days.conform(rows)
    days.write_option_day(day, tmp_path / "20250307.parquet")
    schema = pq.read_schema(tmp_path / "20250307.parquet")
    assert schema.field("bid_px").type == pa.float64() and schema.field("ask_px").type == pa.decimal128(18, 2)
    assert days.read_option_day(tmp_path / "20250307.parquet").equals(day)


def test_synthetic_days_are_reproducible():
    with tempfile.TemporaryDirectory() as tmp:
        runs = {
            name: syn.generate(model, FRIDAY, FRIDAY, Path(tmp) / name, "SPY", overwrite=False)
            for name, model in (("a", SMALL), ("b", SMALL), ("c", replace(SMALL, seed=8)))
        }
        a, b, c = (days.read_option_day(runs[n][0]) for n in "abc")
        assert a.equals(b) and not a.equals(c)


def test_synthetic_quotes_bracket_the_black_price_on_the_tick():
    with tempfile.TemporaryDirectory() as tmp:
        day = days.read_option_day(syn.generate(SMALL, FRIDAY, FRIDAY, Path(tmp), "SPY", False)[0])
    for row in day.sample(200, seed=1).iter_rows(named=True):
        spot = (row["underlying_bid_px"] + row["underlying_ask_px"]) / 2
        s = row["symbol"]
        expiry = syn.engine_expiry(date(2000 + int(s[:2]), int(s[2:4]), int(s[4:6])))
        t = syn.years_to_expiry(row["ts_recv"], expiry)
        strike, is_call = int(s[7:]) / 1000, s[6] == "C"
        fair = syn.price(SMALL, spot, strike, t, is_call)
        assert row["bid_px"] <= fair <= row["ask_px"], row
        assert row["ask_px"] - row["bid_px"] >= SMALL.tick - 1e-9
        for px in (row["bid_px"], row["ask_px"]):
            assert abs(px * 100 - round(px * 100)) < 1e-6, row  # on the cent


def test_synthetic_path_has_the_realized_vol_and_the_planted_momentum():
    def first_and_last_half_hour(model, sessions=400):
        rng, pairs, returns = random.Random(3), [], []
        for _ in range(sessions):
            path = syn.simulate_session(model, rng, 600.0, 391)
            returns += [math.log(b / a) for a, b in itertools.pairwise(path)]
            pairs.append((math.log(path[30] / path[0]), math.log(path[390] / path[360])))
        return pairs, returns

    pairs, returns = first_and_last_half_hour(syn.Model(realized_vol=0.2))
    per_minute = statistics.stdev(returns) / math.sqrt(60 / syn.YEAR_SECONDS)
    assert abs(per_minute - 0.2) < 0.004
    assert abs(statistics.correlation(*zip(*pairs))) < 0.15
    pairs, _ = first_and_last_half_hour(syn.Model(realized_vol=0.2, momentum=1.0))
    assert statistics.correlation(*zip(*pairs)) > 0.6  # 1/sqrt(2) in theory


def test_backtester_reads_back_the_implied_vol():
    if not BACKTEST_DATA.exists():
        print("      (skipped: test_backtest_data not built)")
        return
    # The engine prices a day at the risk-free curve the day carries: at 50%, far from its own default (0.05) so that
    # pricing at the default would miss the IV by about 7e-4, the vol reads back too.
    for flat, day in itertools.product(
        [syn.Model(implied_vol=0.2, tick=0.0, spread=0.0, strikes=3, expiries=3, rate=rate) for rate in (0.05, 0.5)],
        (FRIDAY, date(2025, 8, 4)),  # winter and summer time in New York
    ):
        with tempfile.TemporaryDirectory() as tmp:
            written = syn.generate(flat, day, day, Path(tmp), "SPY", overwrite=False)
            out = subprocess.run(
                [str(BACKTEST_DATA), str(written[0]), "5"],
                capture_output=True,
                text=True,
                check=True,
            ).stdout
        ivs = [float(v) for v in re.findall(r" iv=([0-9.eE+-]+)", out)]
        assert len(ivs) >= 10, out
        assert all(abs(iv - 0.2) < 1e-4 for iv in ivs), (flat.rate, day, ivs)


if __name__ == "__main__":
    failed = 0
    for name, fn in sorted(
        (n, f) for n, f in globals().items() if n.startswith("test_") and callable(f)
    ):
        try:
            fn()
            print(f"ok    {name}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL  {name}: {e}")
    sys.exit(1 if failed else 0)


# ── the example conversion ────────────────────────────────────────────────────

def test_example_conversion_rebuilds_a_day_from_its_csv_files(tmp_path):
    sys.path.insert(0, str(REPO / "scripts" / "data"))
    import example_to_parquet

    model = replace(syn.Model(), strikes=5, expiries=2)
    (day_file,) = syn.generate(model, date(2025, 3, 3), date(2025, 3, 3), tmp_path / "synthetic", "SPY", False)
    day = days.read_option_day(day_file)
    stamp = pl.col("ts_recv").dt.strftime("%Y-%m-%d %H:%M:%S")
    day.select(
        stamp.alias("timestamp"),
        pl.col("symbol").str.slice(0, 6).str.to_date("%y%m%d").alias("expiration"),
        (pl.col("symbol").str.slice(7).cast(pl.Int64) / 1000).alias("strike"),
        pl.col("symbol").str.slice(6, 1).alias("right"),
        pl.col("bid_px").alias("bid"), pl.col("ask_px").alias("ask"),
        pl.col("bid_sz").alias("bid_size"), pl.col("ask_sz").alias("ask_size"),
    ).write_csv(tmp_path / "options.csv")
    day.select(
        stamp.alias("timestamp"), pl.col("underlying_bid_px").alias("bid"), pl.col("underlying_ask_px").alias("ask"),
        pl.col("underlying_bid_sz").alias("bid_size"), pl.col("underlying_ask_sz").alias("ask_size"),
    ).unique(maintain_order=True).write_csv(tmp_path / "underlying.csv")

    out = tmp_path / "converted"
    assert example_to_parquet.main([str(tmp_path / "options.csv"), str(tmp_path / "underlying.csv"),
                                    "--symbol", "SPY", "--out", str(out)]) == 0
    rebuilt = days.day_path(out, "SPY", date(2025, 3, 3))
    assert pq.read_schema(rebuilt).equals(pq.read_schema(day_file))
    assert days.read_option_day(rebuilt).equals(day)


# ── the recorded days ─────────────────────────────────────────────────────────

def test_recorder_writes_each_minute_into_the_day_and_goes_on_after_a_restart(tmp_path):
    sys.path.insert(0, str(REPO / "scripts" / "data"))
    import record_quotes
    from optra.data import recorded_days
    from optra.proto import optra_engine_pb2 as pb2

    open_ns = int(datetime(2026, 10, 13, 9, 30, 20, tzinfo=days.ET).timestamp()) * 10**9
    quotes = {"t": open_ns, "bid": 1.05}

    class Engine:
        def ListPortfolios(self, request, timeout):
            return pb2.ListPortfoliosResponse(portfolios=["SPY", "EMPTY"])

        def GetQuotes(self, request, timeout):
            if request.portfolio == "EMPTY":
                return pb2.QuotesResponse()
            return pb2.QuotesResponse(time_ns=quotes["t"], underlying_bid=663.12, underlying_ask=663.14, options=[
                pb2.OptionQuote(symbol="261013C00663000", bid=quotes["bid"], ask=1.1),
                pb2.OptionQuote(symbol="261013P00663000", bid=0.987654321, ask=1.0)])

    held: dict = {}
    record_quotes.record(Engine(), tmp_path, held)
    record_quotes.record(Engine(), tmp_path, held)  # no quote since: the minute is not written again
    path = days.day_path(tmp_path, "SPY", date(2026, 10, 13))
    assert len(days.read_option_day(path)) == 2
    quotes.update(t=open_ns + 60 * 10**9, bid=1.15)
    record_quotes.record(Engine(), tmp_path, {})  # started again: the day is read from its file
    record_quotes.record(Engine(), tmp_path, {})
    day = days.read_option_day(path)
    assert pq.read_schema(path).names == days.OPTION_ARROW_SCHEMA.names
    assert day["ts_recv"].dt.strftime("%H:%M").to_list() == ["09:31", "09:32", "09:31", "09:32"]
    assert day["bid_px"].to_list() == [1.05, 1.15, 0.987654321, 0.987654321]  # as the engine gave them, exactly
    assert day["underlying_bid_px"].to_list() == [663.12] * 4 and day["bid_sz"].to_list() == [0] * 4
    assert not (tmp_path / "EMPTY").exists()

    with pytest.raises(days.ConversionError, match="not OCC"):
        recorded_days.minute_rows(open_ns, [("SPY", 1.0, 1.1)], 663.0, 663.1)
