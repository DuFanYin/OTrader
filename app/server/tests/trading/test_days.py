"""Tests for the backtest days (optra.services.days, synthetic_days) and the Databento conversions
(scripts/data).

Unit tests use synthetic DBN-like records (numpy arrays with DBN's field names). The round-trip test splits a real day
from data/SPXW back into option and underlying quotes, rebuilds it with
days.convert_frame, and requires the identical file; if the
backtester is built, it also requires an identical backtest result. Those two
are skipped when the data / binary are not present. The synthetic-data tests
check synthetic_days, and, when the backtester's test_backtest_data is
built, that the engine reads back the implied vol the quotes were priced at.
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
from zoneinfo import ZoneInfo

import numpy as np
import polars as pl
import pyarrow.parquet as pq

from optra.services import days
from optra.services import synthetic_days as syn
from optra.services.days import OPTION_SCHEMA

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO / "scripts" / "data"))  # the command lines: dbn, es_to_parquet, options_to_parquet
import es_to_parquet as es  # noqa: E402
import options_to_parquet as opt  # noqa: E402
from dbn import UNDEF_PRICE, regular_session, top_of_book_frame  # noqa: E402
REAL_DAY = REPO / "data" / "SPXW" / "SPXW-2025-08" / "20250804.parquet"
BACKTEST = REPO / "engine" / "build" / "entry_backtest"
BACKTEST_DATA = (
    REPO / "engine" / "build" / "tests" / "checks" / "test_backtest_data"
)
ET = ZoneInfo("America/New_York")


def ns(y, mo, d, h, mi) -> int:
    return int(datetime(y, mo, d, h, mi, tzinfo=ET).timestamp() * 1e9)


# The fields of a CBBO / BBO record the conversion reads, as DBNStore.to_ndarray names them.
RECORD = np.dtype(
    [
        ("ts_recv", "<u8"),
        ("instrument_id", "<u4"),
        ("bid_px_00", "<i8"),
        ("ask_px_00", "<i8"),
        ("bid_sz_00", "<u4"),
        ("ask_sz_00", "<u4"),
    ]
)


def rec(ts, iid=1, bid=1.0, ask=1.2, bsz=5, asz=7) -> tuple:
    px = lambda v: UNDEF_PRICE if v is None else int(round(v * 1e9))  # noqa: E731
    return (ts, iid, px(bid), px(ask), bsz, asz)


def quotes(*recs, start=None, end=None) -> pl.DataFrame:
    return regular_session(top_of_book_frame(np.array(list(recs), dtype=RECORD)), start, end)


def underlying(*times) -> pl.DataFrame:
    recs = [rec(t, iid=9, bid=5000.0, ask=5000.25, bsz=3, asz=4) for t in times]
    return days.load_underlying_frame(es.underlying_quotes([es.priced(quotes(*recs))]))


# ── symbol handling ──────────────────────────────────────────────────────────


def test_symbol_map_strips_root_to_occ_body():
    m = opt.build_symbol_map(
        {"SPXW  250804C05000000": [{"symbol": "11"}], "SPX   250815P04500000": [{"symbol": 12}]}
    )
    assert m == {11: "250804C05000000", 12: "250815P04500000"}


def test_conform_rejects_symbols_the_loader_cannot_parse():
    df = opt.option_quotes(quotes(rec(ns(2025, 8, 4, 10, 0), iid=1)), {1: "SPXW  250804C05000000"})
    try:
        days.conform(days.attach_underlying(df, underlying(ns(2025, 8, 4, 10, 0))))
        raise AssertionError("expected ConversionError")
    except days.ConversionError as exc:
        assert "OCC" in str(exc)


# ── record conversion ─────────────────────────────────────────────────────────


def test_regular_session_bounds_and_undefined_prices():
    times = [
        ns(2025, 8, 4, 9, 29),
        ns(2025, 8, 4, 9, 30),
        ns(2025, 8, 4, 16, 0),
        ns(2025, 8, 4, 16, 1),
    ]
    df = opt.option_quotes(
        quotes(*[rec(t, bid=None if i == 1 else 1.0) for i, t in enumerate(times)]),
        {1: "250804C05000000"},
    )
    assert [t.strftime("%H:%M") for t in df["ts_recv"].to_list()] == ["09:30", "16:00"]
    assert df["bid_px"].to_list() == [None, 1.0]  # UNDEF -> null, not 9.2e9
    assert df.schema["ts_recv"] == OPTION_SCHEMA["ts_recv"]


def test_timestamps_keep_their_nanoseconds():
    t = ns(2025, 8, 4, 10, 0) + 123  # 10:00 and 123 ns
    df = opt.option_quotes(quotes(rec(t)), {1: "250804C05000000"})
    assert df["ts_recv"].dt.epoch("ns").to_list() == [t]
    with tempfile.TemporaryDirectory() as tmp:
        written = days.convert_frame(df, underlying(t), Path(tmp), "SPXW", overwrite=False)
        assert pl.read_parquet(written[0])["ts_recv"].dt.epoch("ns").to_list() == [t]


def test_es_start_end_filter_replaces_hardcoded_february_drop():
    recs = [rec(ns(2025, 2, 28, 10, 0)), rec(ns(2025, 3, 3, 10, 0)), rec(ns(2025, 3, 31, 10, 0))]
    df = quotes(*recs, start=date(2025, 3, 1), end=date(2025, 3, 30))
    assert df["ts_recv"].dt.date().to_list() == [date(2025, 3, 3)]


def test_es_duplicate_timestamps_keep_first():
    t = ns(2025, 3, 3, 10, 0)
    df = es.underlying_quotes([quotes(rec(t, bid=1.0)), quotes(rec(t, bid=2.0))])
    assert df["bid_px"].to_list() == [1.0]


# ── joining, splitting, writing ───────────────────────────────────────────────


def test_missing_underlying_is_an_error_not_a_crash():
    t1, t2 = ns(2025, 8, 4, 10, 0), ns(2025, 8, 4, 10, 1)
    df = opt.option_quotes(quotes(rec(t1), rec(t2)), {1: "250804C05000000"})
    try:
        days.attach_underlying(df, underlying(t1))
        raise AssertionError("expected ConversionError")
    except days.ConversionError as exc:
        assert "1 option timestamps" in str(exc)


def test_worker_reports_errors_instead_of_killing_the_pool():
    # The original raised SystemExit inside the worker, which `except Exception`
    # does not catch and which broke the whole ProcessPoolExecutor.
    path, written, err = opt._worker(
        (Path("/nonexistent.dbn"), Path("/nonexistent.parquet"), Path("/tmp"), "SPXW", False)
    )
    assert written == [] and err


def test_multi_day_file_is_split_into_optra_layout():
    t1, t2 = ns(2025, 8, 29, 15, 59), ns(2025, 9, 2, 9, 31)
    df = opt.option_quotes(quotes(rec(t1), rec(t2)), {1: "250902C05000000"})
    with tempfile.TemporaryDirectory() as tmp:
        written = days.convert_frame(df, underlying(t1, t2), Path(tmp), "SPXW", overwrite=False)
        rel = sorted(str(p.relative_to(tmp)) for p in written)
        assert rel == ["SPXW/SPXW-2025-08/20250829.parquet", "SPXW/SPXW-2025-09/20250902.parquet"]
        out = pl.read_parquet(written[0])
        assert dict(out.schema) == OPTION_SCHEMA
        assert out["underlying_bid_px"].to_list() == [5000.0]
        # existing day files are not silently replaced
        try:
            days.convert_frame(df, underlying(t1, t2), Path(tmp), "SPXW", overwrite=False)
            raise AssertionError("expected ConversionError")
        except days.ConversionError as exc:
            assert "already exists" in str(exc)
        days.convert_frame(df, underlying(t1, t2), Path(tmp), "SPXW", overwrite=True)


# ── round trip on a real day ──────────────────────────────────────────────────


def _backtest(path: Path) -> dict:
    out = subprocess.run(
        [str(BACKTEST), str(path), "StraddleTestStrategy"],
        capture_output=True,
        text=True,
        check=True,
    )
    res = json.loads(out.stdout[out.stdout.index("{") :])["result"]
    return {
        k: res[k]
        for k in (
            "total_rows",
            "total_timesteps",
            "total_orders",
            "final_pnl",
            "net_pnl",
            "max_delta",
        )
    }


def test_real_day_round_trip():
    if not REAL_DAY.exists():
        print("      (skipped: no data/SPXW/SPXW-2025-08/20250804.parquet)")
        return
    original = pl.read_parquet(REAL_DAY)
    option_part = original.select("symbol", "ts_recv", "bid_px", "ask_px", "bid_sz", "ask_sz")
    und = (
        original.select("ts_recv", *[c for c in original.columns if c.startswith("underlying_")])
        .unique(subset=["ts_recv"])
        .rename(lambda c: c.removeprefix("underlying_"))
    )
    with tempfile.TemporaryDirectory() as tmp:
        written = days.convert_frame(
            option_part, days.load_underlying_frame(und), Path(tmp), "SPXW", overwrite=False
        )
        assert [p.name for p in written] == ["20250804.parquet"]
        assert (
            pq.read_schema(written[0])
            .remove_metadata()
            .equals(pq.read_schema(REAL_DAY).remove_metadata())
        ), "Arrow schema differs"
        rebuilt = pl.read_parquet(written[0])
        assert rebuilt.schema == original.schema
        assert rebuilt.equals(original), "rebuilt day differs from data/SPXW"
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
        day = pl.read_parquet(written[0])
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


def test_synthetic_days_are_reproducible():
    with tempfile.TemporaryDirectory() as tmp:
        runs = {
            name: syn.generate(model, FRIDAY, FRIDAY, Path(tmp) / name, "SPY", overwrite=False)
            for name, model in (("a", SMALL), ("b", SMALL), ("c", replace(SMALL, seed=8)))
        }
        a, b, c = (pl.read_parquet(runs[n][0]) for n in "abc")
        assert a.equals(b) and not a.equals(c)


def test_synthetic_quotes_bracket_the_black_price_on_the_tick():
    with tempfile.TemporaryDirectory() as tmp:
        day = pl.read_parquet(syn.generate(SMALL, FRIDAY, FRIDAY, Path(tmp), "SPY", False)[0])
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
    flat = syn.Model(implied_vol=0.2, tick=0.0, spread=0.0, strikes=3, expiries=3)
    for day in (FRIDAY, date(2025, 8, 4)):  # winter and summer time in New York
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
        assert all(abs(iv - 0.2) < 1e-4 for iv in ivs), (day, ivs)


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
