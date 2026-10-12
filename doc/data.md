# Backtest data

A backtest is only as good as the quotes it replays. Optra ships no market data, so this page explains what the backtester expects and the three ways to get it: converting downloads from a data vendor (Databento), replaying the quotes the live engine recorded, and generating synthetic days from a model whose answers are known.

## What the backtester reads

One file per symbol and trading day, in Parquet format:

```
data/SPXW/SPXW-2025-08/20250804.parquet
data/<SYMBOL>/<SYMBOL>-YYYY-MM/YYYYMMDD.parquet
```

The folder name is the portfolio (`SPXW`, `SPY`), and the file name is the day. `data/` is not part of the repository.

Each row is one option's best bid and ask at one moment, together with the underlying's best bid and ask at the same moment:

| Column | Type | Holds |
|---|---|---|
| `symbol` | string | the option, as an OCC symbol without its root, e.g. `250804C05000000` |
| `ts_recv` | timestamp (New York) | when the quote was seen |
| `bid_px`, `ask_px`, `bid_sz`, `ask_sz` | double, int64 | the option's best bid and ask, and their sizes |
| `underlying_bid_px`, `underlying_ask_px`, `underlying_bid_sz`, `underlying_ask_sz` | double, int64 | the underlying's |

The symbol carries the option's identity: the expiry date `YYMMDD`, `C` or `P`, and the strike times 1000 in eight digits. `250804C05000000` is a call expiring on 2025-08-04 with a strike of 5000. The root (`SPXW`) is left out of the symbol, because it comes from the folder.

Implied volatilities and Greeks are not stored. The engine works them out from the quotes as it replays them, the same way it does live. A file with the wrong column types (an integer timestamp, say) is rejected with the reason in the backtest's errors, and that day is skipped rather than run with bad data.

Any data vendor works, as long as the result has this shape: one row per option and moment, with the underlying's quote joined on, one file per day.

## From Databento

The `data/SPXW` days were made from Databento downloads: SPXW option quotes (one-minute best bid and offer, from OPRA) and, as the underlying, the E-mini S&P 500 future's quotes. The conversion has three steps, all run from the repository root in the server's Python environment:

```bash
P="uv run --project app/server --group databento python scripts/data"

# 1. The underlying: the futures quotes, regular session only (09:30-16:00 New York), into one file
$P/es_to_parquet.py data/raw/SPX-data/es_mini -o data/raw/SPX-data/es_mini/es_continuous_bbo_1m_rth.parquet --start 2025-03-01

# 2. The options: joined with the underlying minute by minute, one file per trading day
$P/options_to_parquet.py data/raw/SPX-data --underlying data/raw/SPX-data/es_mini/es_continuous_bbo_1m_rth.parquet \
    --out data --symbol SPXW --workers 4 > data/raw/convert.log 2>&1

# 3. Check the result, then backtest a day
$P/inspect_parquet.py data/SPXW/SPXW-2025-08 --head 0
./engine/build/entry_backtest --files data/SPXW/SPXW-2025-08/20250804.parquet IvMeanRevertStrategy
```

A day of SPXW takes about 4 seconds and 1 GB of memory per worker: four workers on a 13 GB machine converted 124 days in under four minutes. A day that cannot be converted (an option quote with no underlying quote at that minute, or a symbol that is not a valid OCC symbol) is reported and skipped; the others go on, and existing days are never overwritten unless you ask. The day 2025-08-04 has about 5.2 million rows for 16,202 contracts over 390 minutes, and backtests in about 3 seconds.

Two things to know about this data:

- **The underlying is a futures price, not the S&P 500 index.** The future is what you would hedge with, but its price includes carry to its own expiry. The engine values each option expiry against its own forward price (from put-call parity), so this does not bias implied volatility or delta. Delta is measured against the future.
- **The futures series rolls.** The continuous contract switches to the next future on roll days (for example 2025-03-19), and the price jumps by the difference between the two at that minute.

## Replaying a paper day

The live engine can record each portfolio's quotes, a snapshot at the end of every minute, one CSV file a day (set `OPTRA_RECORD_DIR`). Those recordings convert into backtest days, so a day of paper trading can be replayed in the backtester and the two compared fill by fill:

```bash
uv run --project app/server python scripts/data/recorded_to_parquet.py data/recorded/SPY --out data/recorded-parquet
```

The end-of-day report does this on its own, and so does the screener when it reads a recorded day as an option chain. Recordings carry no quote sizes, so those are written as zero.

## Synthetic days

Synthetic days are generated from a model, so you know exactly what is in them. They are useful for checking that a strategy finds an edge that is really there, and that it finds nothing when nothing is there.

```bash
uv run --project app/server python scripts/data/synthetic_to_parquet.py --start 2025-03-03 --end 2025-03-07 --out data --symbol SPY
./engine/build/entry_backtest --files data/SPY/SPY-2025-03/*.parquet IvMeanRevertStrategy
```

The page and the server can make them too (`POST /api/data/synthetic`).

The underlying follows a random walk (geometric Brownian motion) minute by minute through each weekday's session, with a gap overnight. Each day lists the nearest expiries and a range of strikes around the open, and every option is quoted around its Black price at the implied volatility you choose. The prices are computed exactly the way the engine reads them back, so with no tick rounding and no spread the engine recovers precisely the implied volatility you put in. The same arguments always produce the same files, and the arguments are saved next to them.

What you can plant, for a strategy to find:

| Setting | Plants |
|---|---|
| implied volatility above realised volatility | options are expensive: selling them and hedging should earn the difference |
| skew and smile | a volatility smile in the quotes |
| momentum | the last half hour of each day continues the first half hour's move |

With nothing planted, a strategy should lose roughly its trading costs. One that makes money anyway is exploiting the backtester or the quote model, not a real edge.

## Where the code is

The format and its checks are in `app/server/optra/services/days.py`; every day, from any source, is written through the same function, so all of them pass the same checks. Synthetic and recorded days are `synthetic_days.py` and `recorded_days.py` next to it. The command lines are in `scripts/data/`, and the tests in `app/server/tests/trading/test_days.py` (including a round trip that rebuilds a real day from its own columns and requires an identical file and an identical backtest).
