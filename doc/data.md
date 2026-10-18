# Backtest data

A backtest is only as good as the quotes it replays. Optra ships no market data. This page explains what the backtester reads, how a day is stored, and the three ways to get days: converting a data vendor's data, replaying what the live engine recorded, and generating synthetic days from a known model.

## One file a day

The backtester reads one Parquet file per symbol and trading day:

```
data/<SYMBOL>/<SYMBOL>-YYYY-MM/YYYYMMDD.parquet
data/SPXW/SPXW-2025-08/20250804.parquet
```

The folder is the portfolio (`SPXW`, `SPY`); the file name is the day. `data/` is not part of the repository.

Each row is one option's best bid and ask at one moment, with the underlying's best bid and ask at the same moment joined on:

| Column | Type | Holds |
|---|---|---|
| `symbol` | string | the option, e.g. `250804C05000000` |
| `ts_recv` | timestamp (New York) | when the quote was seen |
| `bid_px`, `ask_px` | decimal or double | the option's best bid and ask |
| `bid_sz`, `ask_sz` | int64 | their sizes |
| `underlying_bid_px`, `underlying_ask_px` | double | the underlying's best bid and ask |
| `underlying_bid_sz`, `underlying_ask_sz` | int64 | their sizes |
| `open_interest` (optional) | int64 | each contract's open interest as of the open |

The symbol is an OCC symbol without its root: the expiry `YYMMDD`, `C` or `P`, and the strike times 1000 in eight digits. `250804C05000000` is a call expiring on 2025-08-04 with a strike of 5000. The root comes from the folder.

Implied volatilities and Greeks are not stored. The engine works them out from the quotes as it replays them, the same way it does live. The engine skips any column it does not know. A file with the wrong column types is rejected, with the reason in the backtest's errors, and that day is skipped rather than run on bad data.

### Prices

Data vendors send a price as an integer and a number of decimal places. A file keeps it that way, as a decimal, and the engine reads it as the integer times 10 to the minus its places, in double arithmetic. That is the same double the vendor's own client makes of it: 9332 at two places is 9332 × 0.01, which is 93.32000000000001. A price can also be a plain double.

### The risk-free rate

A day can carry its risk-free curve in the file's metadata, under `risk_free_curve`:

```json
{"years": [0.083, 0.25, 0.5, 1, 2], "rates": [...]}
```

The rates are continuously compounded, a year, at maturities in years. The engine prices each expiry at the rate to it on the curve (a straight line between the points, flat beyond the ends) and gives strategies the same rate (`Chain::rate`). A day without a curve is priced at the one rate the backtest is given (5% by default).

The converted days carry the US Treasury curve of the business day before, which is known at the open. Synthetic days carry the rate they were priced at.

## How a day is stored

Every conversion writes a day through the same function, `days.write_option_day`:

- Each option's rows together, by symbol and then time. The engine does not need the rows in time order; it sorts them as it reads.
- One row group per expiry, so a backtest reads only the expiries it asks for.
- The option's prices as decimals with the fewest places, from 2 to 9, that hold every one of them exactly. When no number of places does, they stay doubles.
- The time and the decimal prices delta-encoded, the other columns by dictionary, zstd at level 9.

Nothing is lost. The file is read back, an expiry at a time, and compared with what was written, value for value, before it replaces anything. On whole-chain days the result is about 60% of the same rows written by symbol with zstd at level 3, and about a quarter of the rows written in time order with snappy.

## How a backtest reads a day

A file can hold the whole chain: every expiry and every strike. A backtest does not read it all. It first reads the day's contracts and minutes. Then it reads a chain (one expiry) only when a strategy subscribes to it, holds one of its options or orders one, the way live trading streams only what it is asked for. A chain is read and priced once for all the backtests running on that day.

Both parts are kept in the day cache (`data/cache/days/<SYMBOL>/`):

- `<day>-<key>.day`: the day's contracts and minutes.
- `<day>-<chain>-<key>.chain`: one chain, priced.

The cache keeps prices and sizes exactly, and implied volatilities, but no Greeks; those are priced on demand by the same code as live. A `.chain` is laid out as a backtest reads it, minute by minute, so it is mapped and read where it is, not decoded or copied. A result from the cache is bit for bit the result without it.

A `.day` is keyed by the file's name, size and time written. A `.chain` is keyed also by what it was priced with: the pricing code, the rate and the price the IV is taken from. A change to any of them makes a new entry, and a change to other code keeps the old ones. Over its budget (`day_cache_gb` in the server's config, 100 GB by default) the cache drops the files read longest ago first.

For example, 22 SPXW days with three strategies on three chains take about 10 s with an empty cache and 1 s from a filled one. A new strategy on another chain reads the `.day` files from the cache and prices only its new chain.

## From a data vendor

Optra is bound to no data vendor. Whatever a vendor delivers is kept as it came and converted offline, by a script, into the days above. The engine reads only those, so a new vendor means a new conversion, never a change to the system.

Every conversion goes through `days.convert_frame` in `app/server/optra/data/days.py`. It joins the underlying's quote onto each option row, writes one file per trading day and checks the result against the format.

`scripts/data/example_to_parquet.py` is a small example. It converts quotes that come as two CSV files, the options and the underlying, with a timestamp, expiry, strike, right, bid, ask and sizes per row:

```bash
uv run --project app/server python scripts/data/example_to_parquet.py options.csv underlying.csv --symbol SPY --out data
```

Data in another shape needs only another way of reading it into the same columns.

## Values by day

Besides the days, a strategy may read values that come once a day: an index's close, an implied volatility at the close, the dates of events. Each series is one file, `data/daily/<series>.csv`, a line `YYYY-MM-DD,value` each (a header line and lines starting with `#` are skipped); an event series lists its dates. Backtest and live read the same files, and a strategy sees each value from the next day on (`Strategy::daily`, `daily_history`, `trading_days_until`), so it never reads a close before it happened. The live engine reads them again each morning; a backtest's trials hash them, so changing a file makes new trials. The series `<underlying>_ex_dividend` (the ex-dividend dates of a share-settled underlying) has the risk checks close any combo with a call sold before the close of the day before. One series the system works out from the days themselves: `<PORTFOLIO>_minute_rv`, the realized vol of the underlying's minutes, which `ComboStrategy` filters on (`python -m optra.data.daily_values data/<PORTFOLIO>` over the backtest days; the end-of-day report adds each paper day from the day recorded). Read from the file, not from the minutes a strategy saw, it is the same after a restart during the day.

## Replaying a paper day

A small recorder outside the engine (`scripts/data/record_quotes.py`) asks the running engine for each portfolio's quotes just after every minute and adds them to that day's backtest file, `data/recorded/<PORTFOLIO>/<PORTFOLIO>-YYYY-MM/YYYYMMDD.parquet`, in the same layout and schema as any other day. So a day of paper trading can be replayed in the backtester and compared fill by fill as it is, with nothing to convert: the end-of-day report does that, and the screener reads today's day as an option chain as of the last minute recorded. The day is written whole again each minute and read back before it replaces the file, so a reader always sees a complete day; a recorder started again reads the day from its file and goes on. Recordings carry no quote sizes, so those are written as zero.

## Synthetic days

Synthetic days are generated from a model, so you know exactly what is in them. They check that a strategy finds an edge that is really there, and finds nothing when nothing is there.

```bash
uv run --directory app/server python -m optra.data.synthetic_days --start 2025-03-03 --end 2025-03-07 --out ../../data/synthetic --symbol SPY
./engine/build/entry_backtest --files data/synthetic/SPY/SPY-2025-03/*.parquet ComboStrategy
```

The page and the server can make them too (`POST /api/data/synthetic`). They go to `data/synthetic`, apart from the real days in `data/<SYMBOL>`: research takes every day there is for an underlying, and a synthetic day among them would count as a real one.

The model:

- The underlying follows a random walk (geometric Brownian motion) minute by minute through each weekday's session, with a gap overnight.
- Each day lists the nearest expiries and a range of strikes around the open.
- Every option is quoted around its Black price at the implied volatility you choose. The prices are computed exactly the way the engine reads them back, so with no tick rounding and no spread the engine recovers exactly the implied volatility you put in.
- The same arguments always produce the same files, and the arguments are saved next to them.

What you can plant for a strategy to find:

| Setting | Plants |
|---|---|
| implied volatility above realised volatility | options are expensive: selling them and hedging should earn the difference |
| skew and smile | a volatility smile in the quotes |
| momentum | the last half hour of each day continues the first half hour's move |

With nothing planted, a strategy should lose about its trading costs. One that makes money anyway is exploiting the backtester or the quote model, not a real edge.

## Where the code is

- The format, its checks and the writer: `app/server/optra/data/days.py`. Every day, from any source, is written through it, so all of them pass the same checks.
- Synthetic and recorded days: `synthetic_days.py` and `recorded_days.py` next to it (the recorder adds each minute through `recorded_days.py`).
- The command lines: `scripts/data/`.
- The tests: `app/server/tests/trading/test_days.py`, including a round trip that rebuilds a real day from its own columns and requires an identical file and an identical backtest.
- Reading a day and the cache: `engine/adapters/market_data/` (`data_loader`, `historical_data`).
