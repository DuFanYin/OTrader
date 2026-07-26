# Preparing backtest data

OTrader ships **no market data** — you bring your own. The backtester reads Parquet files with a fixed schema, laid out by symbol. This repo's samples were cleaned from [Databento](https://databento.com/) DBN files, but any source works as long as the output matches the schema below.

## Layout

```
data/
  <SYMBOL>/
    YYYYMMDD.parquet     # one file per trading day
```

- `<SYMBOL>` is the portfolio/underlying name you pass to the backtester (e.g. `SPXW`).
- Files are discovered recursively under `data/<SYMBOL>/`; the date is parsed from the `YYYYMMDD.parquet` filename.

Run a single day directly, or a date range by symbol:

```bash
./Otrader/build/entry_backtest data/SPXW/SPXW-2025-08/20250804.parquet StraddleTestStrategy
```

## Parquet schema

One row = one option quote at one timestamp. Columns (exact names, all required):

| Column | Type | Meaning |
|--------|------|---------|
| `symbol` | string | OCC option symbol, e.g. `SPXW  250804C05000000` (see below) |
| `ts_recv` | timestamp | Quote time — the loader's default time column (`ts_recv`); any Arrow time unit is accepted |
| `bid_px` / `ask_px` | double | Option best bid / ask price |
| `bid_sz` / `ask_sz` | int64 | Option best bid / ask size |
| `underlying_bid_px` / `underlying_ask_px` | double | Underlying best bid / ask price |
| `underlying_bid_sz` / `underlying_ask_sz` | int64 | Underlying best bid / ask size |

Strike, expiry, and call/put are **not** separate columns — they are parsed from the OCC `symbol` (`utilities/occ_utils`). IV and Greeks are computed on the fly from these quotes (Black-Scholes), not stored.

### OCC symbol format

The strike/expiry/right encoding follows the OCC convention, e.g. `250804C05000000`:

```
YYMMDD  C/P  strike*1000 (8 digits)
250804   C   05000000     -> 2025-08-04, Call, strike 5000.0
```

## Cleaning from Databento DBN

There is **no cleaning script in this repo** — the source-to-Parquet step is yours to write, and it is where most of the work is. A typical pipeline:

1. Pull the option chain + underlying quotes for your symbol/day from Databento (or another vendor).
2. Normalize to one row per (option, timestamp) with the columns above; join the underlying's best bid/ask onto each option row so `underlying_*` is populated per row.
3. Encode each option's identity as an OCC `symbol` string.
4. Write one Parquet file per trading day at `data/<SYMBOL>/YYYYMMDD.parquet`.

Keep files per-day: the backtester treats each file as an independent replay and can run multiple days in parallel.
