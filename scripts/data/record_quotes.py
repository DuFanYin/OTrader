"""Record the live engine's quotes a minute at a time, as backtest days, to replay a paper day.

Outside the engine: just after each minute it asks the running engine (gRPC GetQuotes) for each
portfolio's quotes as it has them, the latest snapshot applied, and adds them to the portfolio's
day: OUT/<PORTFOLIO>/<PORTFOLIO>-YYYY-MM/YYYYMMDD.parquet (the New York date), the backtest days'
layout and schema, each quote stamped with the end of its minute (optra.data.recorded_days). A
minute with no new quotes is not written again. When the engine cannot be reached it waits for the
next minute.

Usage (from the repository root; optra-recorder.service runs it):
  uv run --project app/server python scripts/data/record_quotes.py --out data/recorded
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from datetime import date
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "app" / "server"))  # the optra package
import grpc  # noqa: E402
import polars as pl  # noqa: E402

from optra.data import recorded_days  # noqa: E402
from optra.data.days import ConversionError  # noqa: E402
from optra.proto import optra_engine_pb2 as pb2  # noqa: E402
from optra.proto import optra_engine_pb2_grpc as pb2_grpc  # noqa: E402

MINUTE_NS = 60_000_000_000


def minute_end(time_ns: int) -> int:
    """The end of the minute time_ns falls in (a time on a minute is that minute's end)."""
    return -(-time_ns // MINUTE_NS) * MINUTE_NS


def record(stub: pb2_grpc.EngineServiceStub, out: Path,
           held: dict[str, tuple[date, pl.DataFrame]]) -> None:
    """Each portfolio's quotes, when they are of a minute its day does not have yet."""
    for portfolio in stub.ListPortfolios(pb2.Empty(), timeout=10).portfolios:
        quotes = stub.GetQuotes(pb2.QuotesRequest(portfolio=portfolio), timeout=10)
        if quotes.time_ns == 0 or not quotes.options:
            continue
        try:
            minute = recorded_days.minute_rows(
                minute_end(quotes.time_ns), [(o.symbol, o.bid, o.ask) for o in quotes.options],
                quotes.underlying_bid, quotes.underlying_ask)
            recorded_days.add_minute(held, out, portfolio, minute)
        except ConversionError as e:  # this minute is left out; the day goes on
            print(f"record_quotes: {portfolio}: {e}", flush=True)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--out", type=Path, required=True, help="where the days go (OUT/PORTFOLIO/...)")
    ap.add_argument("--target", default=os.getenv("LIVE_GRPC_TARGET", "localhost:50051"),
                    help="the engine's gRPC address (default: $LIVE_GRPC_TARGET, else localhost:50051)")
    args = ap.parse_args(argv)

    stub = pb2_grpc.EngineServiceStub(grpc.insecure_channel(
        args.target, options=[("grpc.max_receive_message_length", 64 * 1024 * 1024)]))
    held: dict[str, tuple[date, pl.DataFrame]] = {}
    unreachable = False
    while True:
        # Just after the minute: its last quotes are in.
        time.sleep(60 - time.time() % 60 + 0.5)
        try:
            record(stub, args.out, held)
            if unreachable:
                print("record_quotes: the engine answers again", flush=True)
            unreachable = False
        except grpc.RpcError as e:
            if not unreachable:
                print(f"record_quotes: the engine does not answer ({e.code().name}); waiting",
                      flush=True)
            unreachable = True


if __name__ == "__main__":
    sys.exit(main())
