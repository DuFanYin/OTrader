"""Tests for the data-prep tools.

    python -m pytest scripts/data_prep/test_data_prep.py      (or run this file directly)

Unit tests use synthetic DBN-like records. The round-trip test splits a real day
from data/SPXW back into option and underlying quotes, rebuilds it with
options_to_parquet.convert_frame, and requires the identical file; if the
backtester is built, it also requires an identical backtest result. Those two
are skipped when the data / binary are not present.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from datetime import date, datetime
from pathlib import Path
from types import SimpleNamespace
from zoneinfo import ZoneInfo

import polars as pl
import pyarrow.parquet as pq

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import es_to_parquet as es  # noqa: E402
import options_to_parquet as opt  # noqa: E402
from dbn_common import OPTION_SCHEMA, UNDEF_PRICE  # noqa: E402

REPO = HERE.parents[1]
REAL_DAY = REPO / "data" / "SPXW" / "SPXW-2025-08" / "20250804.parquet"
BACKTEST = REPO / "Otrader" / "build" / "entry_backtest"
ET = ZoneInfo("America/New_York")


def ns(y, mo, d, h, mi) -> int:
    return int(datetime(y, mo, d, h, mi, tzinfo=ET).timestamp() * 1e9)


def rec(ts, iid=1, bid=1.0, ask=1.2, bsz=5, asz=7):
    px = lambda v: UNDEF_PRICE if v is None else int(round(v * 1e9))  # noqa: E731
    return SimpleNamespace(
        ts_recv=ts,
        instrument_id=iid,
        levels=[SimpleNamespace(bid_px=px(bid), ask_px=px(ask), bid_sz=bsz, ask_sz=asz)],
    )


def underlying(*times) -> pl.DataFrame:
    rows = es.records_to_rows([rec(t, iid=9, bid=5000.0, ask=5000.25, bsz=3, asz=4) for t in times])
    return opt.load_underlying_frame(es.to_frame(rows))


# ── symbol handling ──────────────────────────────────────────────────────────


def test_symbol_map_strips_root_to_occ_body():
    m = opt.build_symbol_map(
        {"SPXW  250804C05000000": [{"symbol": "11"}], "SPX   250815P04500000": [{"symbol": 12}]}
    )
    assert m == {11: "250804C05000000", 12: "250815P04500000"}


def test_conform_rejects_symbols_the_loader_cannot_parse():
    df = opt.records_to_frame([rec(ns(2025, 8, 4, 10, 0), iid=1)], {1: "SPXW  250804C05000000"})
    try:
        opt.conform(opt.attach_underlying(df, underlying(ns(2025, 8, 4, 10, 0))))
        raise AssertionError("expected ConversionError")
    except opt.ConversionError as exc:
        assert "OCC" in str(exc)


# ── record conversion ─────────────────────────────────────────────────────────


def test_regular_session_bounds_and_undefined_prices():
    times = [
        ns(2025, 8, 4, 9, 29),
        ns(2025, 8, 4, 9, 30),
        ns(2025, 8, 4, 16, 0),
        ns(2025, 8, 4, 16, 1),
    ]
    df = opt.records_to_frame(
        [rec(t, bid=None if i == 1 else 1.0) for i, t in enumerate(times)], {1: "250804C05000000"}
    )
    assert [t.strftime("%H:%M") for t in df["ts_recv"].to_list()] == ["09:30", "16:00"]
    assert df["bid_px"].to_list() == [None, 1.0]  # UNDEF -> null, not 9.2e9
    assert df.schema["ts_recv"] == OPTION_SCHEMA["ts_recv"]


def test_es_start_end_filter_replaces_hardcoded_february_drop():
    recs = [rec(ns(2025, 2, 28, 10, 0)), rec(ns(2025, 3, 3, 10, 0)), rec(ns(2025, 3, 31, 10, 0))]
    rows = es.records_to_rows(recs, start=date(2025, 3, 1), end=date(2025, 3, 30))
    assert [r["ts_recv"].date() for r in rows] == [date(2025, 3, 3)]


def test_es_duplicate_timestamps_keep_first():
    t = ns(2025, 3, 3, 10, 0)
    df = es.to_frame(es.records_to_rows([rec(t, bid=1.0), rec(t, bid=2.0)]))
    assert df["bid_px"].to_list() == [1.0]


# ── joining, splitting, writing ───────────────────────────────────────────────


def test_missing_underlying_is_an_error_not_a_crash():
    t1, t2 = ns(2025, 8, 4, 10, 0), ns(2025, 8, 4, 10, 1)
    df = opt.records_to_frame([rec(t1), rec(t2)], {1: "250804C05000000"})
    try:
        opt.attach_underlying(df, underlying(t1))
        raise AssertionError("expected ConversionError")
    except opt.ConversionError as exc:
        assert "1 option timestamps" in str(exc)


def test_worker_reports_errors_instead_of_killing_the_pool():
    # The original raised SystemExit inside the worker, which `except Exception`
    # does not catch and which broke the whole ProcessPoolExecutor.
    path, written, err = opt._worker(
        (Path("/nonexistent.dbn"), Path("/nonexistent.parquet"), Path("/tmp"), "SPXW", False)
    )
    assert written == [] and err


def test_multi_day_file_is_split_into_otrader_layout():
    t1, t2 = ns(2025, 8, 29, 15, 59), ns(2025, 9, 2, 9, 31)
    df = opt.records_to_frame([rec(t1), rec(t2)], {1: "250902C05000000"})
    with tempfile.TemporaryDirectory() as tmp:
        written = opt.convert_frame(df, underlying(t1, t2), Path(tmp), "SPXW", overwrite=False)
        rel = sorted(str(p.relative_to(tmp)) for p in written)
        assert rel == ["SPXW/SPXW-2025-08/20250829.parquet", "SPXW/SPXW-2025-09/20250902.parquet"]
        out = pl.read_parquet(written[0])
        assert dict(out.schema) == OPTION_SCHEMA
        assert out["underlying_bid_px"].to_list() == [5000.0]
        # existing day files are not silently replaced
        try:
            opt.convert_frame(df, underlying(t1, t2), Path(tmp), "SPXW", overwrite=False)
            raise AssertionError("expected ConversionError")
        except opt.ConversionError as exc:
            assert "already exists" in str(exc)
        opt.convert_frame(df, underlying(t1, t2), Path(tmp), "SPXW", overwrite=True)


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
        written = opt.convert_frame(
            option_part, opt.load_underlying_frame(und), Path(tmp), "SPXW", overwrite=False
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
            print("      (backtest comparison skipped: Otrader/build/entry_backtest not built)")


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
