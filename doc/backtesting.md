# Efficient Option Strategy Backtesting

Most of an options backtest's time is spent reading and pricing data. A day of SPY lists about 9,000 options in 37 expiries, quoted every minute, and each option's implied volatility is solved before a strategy runs. Research runs hundreds of settings, a control and robustness checks over six years of days, so the same days are read many times.

The rules:

- the backtest runs the live engine's code;
- no format loses a bit;
- a day is read and priced once for every trial;
- a run's days are summed in one place.

Numbers are from one shared laptop (an 8-core Ryzen 7 5850U).

## Three tiers of storage

| Tier | What | SPY |
|---|---|---|
| Downloads | as delivered, read only to convert | 2,697 days from 2016, 47 GB, about 6 bytes an option-minute |
| Backtest days | one Parquet file a day, one row group per expiry (a backtest skips what it does not trade), exact decimal prices, dictionary symbols, zstd; each read back and compared when written | 30 GB, about 4 bytes an option-minute |
| Day cache | what the first read priced: a `.day` (contracts and minutes, about 1 MB) and a `.chain` per expiry used (quotes, forwards, IVs, each minute's smile), laid out as the backtest reads it | 3.7 MB for the same-day chain; loading one is mapping a file, 0.04 ms |

Greeks are not cached: the live code prices them on demand, bit for bit the same. The format in full is on the [data page](data.md).

## Reading a day, running many trials

- **Only what is asked for**: a day's contracts and minutes, then a chain only when a strategy subscribes to it, holds or orders one of its options, as live streams only what it is asked for.
- **Priced once**: a chain read from the day file gets its forward from put-call parity, then each option's IV, then each minute's smile, once.
- **Read ahead**: a loader reads the next days while the current one runs.
- **Shared, read only**: each trial has its own engine (the live engine with a backtest venue in place of the broker), but no trial reads or prices a day; each day is loaded once and shared.
- **One replay per thread**: the trials on a thread share a replay that rolls each day in once and applies each minute once, copying only the rows of the chains its engines asked for, not those read ahead for them. A day is freed on a loader thread once the last trial lets go of it, off the trials' way.
- **Deterministic**: one thread owns each engine's state, so a trial gives the same result alone or beside others.
- **Fills**: at the next minute's quotes, at a set position in each spread, with fees, size limits or delays defined once on the server.
- **Summed once**: each trial writes its days and round trips as columns; the server alone sums them, in one function for the ledger, the tools and the page, so every view shows the same Sharpe ratio and drawdown. Jobs over the same days run as one batch: the experiments one request makes (a strategy and its control, the robustness checks, a report's strategies) are queued in one transaction, so a waiting worker takes them together and reads each day once.

## Measure first

A profile of one trial over 200 cached SPY days: the strategy took under 1% of the time; the rest was preparing each day.

| Each day, SPY | Before | After | Change |
|---|---|---|---|
| Loading a day: contracts, listing, minutes | 11 ms | 2.2 ms | listing built in one pass from contracts stored in its order |
| Loading the same-day chain | 9 ms | 0.04 ms | the chain read in place, mapped |
| Rolling it into a thread group's portfolio | 2.2 ms | 0.2 ms | the roll walks two sorted listings side by side |
| Letting go of the day before | 2.6 ms | 0.4 ms | |
| `malloc_trim`, then faulting the memory back in | 1.8 ms, half the CPU in the kernel | none | the daily `malloc_trim` removed |

Old and new engines, alternated on the same busy machine:

| SPY, 200 days, from the cache | Before | After |
|---|---|---|
| 1 trial | 4.4–5.5 s | 1.15–1.4 s |
| 128 trials | 7.4–8.2 s, 350–380 MB | 4.1–4.8 s, 260–270 MB |
| 512 trials | 16.8–17.3 s, 500–530 MB | 12.3–12.9 s, 450–460 MB |

- Uncached days did not get slower; every result came out equal, value for value.
- With days cheap, the engines' own work each minute is now two thirds of a large batch: the next thing to measure.

More in [engine.md](engine.md#backtests) and on the [architecture page](https://dufanyin.dev/optra-architecture/).
