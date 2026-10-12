# The engine

The engine is the part of Optra that trades. It is written in C++23, and the same code runs a backtest and live trading on Interactive Brokers. This page explains how it works: how a strategy sees the market, how orders and fills are booked, how the live process is organised, how it survives restarts, and how a backtest differs. The last sections cover what Interactive Brokers actually sends back, performance, building and testing.

For how the engine fits with the rest of Optra, start with [architecture.md](architecture.md).

## Events in, orders out

A strategy never calls the outside world. It reacts to *events* and produces *intents*.

There are four kinds of event:

- a **snapshot**: the latest prices of a portfolio's options and underlying, with each option's implied volatility and Greeks already worked out;
- a **timer** tick, once per bar in a backtest and once a second live;
- an **order update**: an order was accepted, filled, cancelled or rejected;
- a **fill**: some quantity of one contract traded at a price.

And there are three kinds of intent: send an order, cancel an order, write a log line.

Everything between the two is the engine's job. When a snapshot arrives, the engine updates the portfolio's prices and tells the strategies trading it. On each timer tick it first settles any options that have expired, then runs the strategies, then refreshes each strategy's position metrics, then runs the delta hedges. When an order update or a fill arrives, it books it and then tells the strategy that owns the order.

This is written once (in `core::Engine`) and used by both the backtest and live trading. The engine does not know which mode it is in. The difference lies only in what is plugged into it: where events come from, what the clock is, where orders go, and where records are kept.

| Plugged in | Backtest | Live |
|---|---|---|
| Quotes | a day's file of recorded quotes | Interactive Brokers, through the market-data connection |
| Clock | the time of the current bar | the wall clock |
| Orders go to | a simulated venue that fills at the next bar | Interactive Brokers |
| Records kept in | an in-memory database | `data/optra.db` |

## Writing a strategy

A strategy is a C++ class with a handful of callbacks: when it starts, on each snapshot, on each timer tick, on each order update and fill. It declares its parameters (a name, a default, a range), so the page and the server can show and validate them without knowing anything about the strategy. It reads the time with `now()`, never by counting ticks, so the same code works whether a bar is a minute in a backtest or a second live.

A strategy sees the engine through a narrow view: its portfolio's prices, its own position, its working orders, and the ability to send orders. It cannot reach the database, the network or other strategies.

The most general strategy is **ComboStrategy**. It trades one combination of options described by rules rather than by contracts: each leg's ratio, call or put, strike as a percentage away from its expiry's forward price, and days to expiry. It opens at a set minute, closes at another, and can close early at a profit or loss measured against the premium. It can optionally delta-hedge, and it can optionally open only when a Heston model calibrated to the day's options says the combination is cheap. This is the strategy the research tools hand trade ideas to.

## Booking orders and fills

Getting positions right is the engine's most important job, and three rules make it reliable.

**A fill is always one contract's.** When a combination of options trades, the engine books each leg separately. A combination's position is then just its legs, and its quantity follows from theirs. This keeps the bookkeeping identical for single options and combinations.

**The order of reports does not matter.** Brokers report order updates and fills in whatever order they like: an order may be reported "filled" before its fills arrive. The engine merges every update into the order, ignores ones that are stale or repeated, and holds back the news that an order is finished until its fills have been booked. A strategy that hears "your order is done" can therefore trust its position. A fill that arrives twice is booked once.

**Profit and loss is explained, not just reported.** Between updates, each strategy's P&L is split by where it came from: the underlying's move (delta), the curvature of that move (gamma), time passing (theta), changes in volatility (vega), the prices paid against the middle of the market at each fill, and whatever is left over. The parts add up to the total, so a strategy that makes money can be checked for *why* it did.

Fees follow Interactive Brokers: a fee per option contract, and for shares a fixed 0.005 USD each, at least 1 USD an order.

Every order also passes a **risk check** before it leaves: limits on order size, position size and premium, a price band around the market, a maximum quoted spread for market orders, no naked short options, and only closing orders shortly before an expiring option that settles in shares. The operator can halt everything except orders that only reduce positions. The same checks run in backtests.

## The live engine

Live, the engine is one process that runs all day. Inside it are a few threads, each with one job:

| Thread | Job |
|---|---|
| the event thread | runs every strategy and books every order and fill; the only thread that changes trading state |
| the IB order thread | receives order updates and fills from Interactive Brokers, and writes orders out to the network |
| the IB market-data thread | receives quotes and turns each batch into fresh, priced snapshots |
| the log thread | writes log lines out |
| the database thread | saves orders, fills and positions in batches |
| the gRPC threads | answer the server's requests |

The most important choice is the first row: **one thread owns all trading state**. Strategies, booking, positions and hedging all run there, one event after another. There are no locks around positions or orders, and events happen in the same order as they would in a backtest. The price of this choice is that the event thread must never be slowed down by the outside world, so it never waits: writing to the network, to the log and to the database is all handed to other threads.

How events reach the event thread depends on what they are:

- **Order updates, fills and commands** join an ordered queue that never drops anything and never makes the sender wait. Losing a fill would be a bookkeeping error, so this queue has no size limit.
- **Snapshots** go into one slot per portfolio. A new snapshot replaces one that has not been used yet. If quotes arrive faster than the strategies can react, they simply see the latest prices, and a burst of quotes can never push a fill back in the queue.
- **Log lines** go through a bounded buffer. If it ever overflows, log lines are dropped and counted, rather than slowing anything down.

When a thread has nothing to do it sleeps, and a producer only wakes it when it is actually asleep. The server's commands ("start this strategy", "list the positions") are run on the event thread too, so they see a consistent state.

**Stopping** is orderly. The server's connection is closed, the IB connections stop (orders not yet sent are reported as rejected), the event thread finishes what is already queued, and the database is closed. Strategies are *not* stopped, because a strategy's stop routine may close positions. They are simply resumed on the next start.

## Restarts, expiry and the trading day

The engine is restarted every morning, and it always resumes where it stopped. Every order, fill and strategy setting is saved as it happens.

**On start**, the engine adds back each strategy under its old name with its old settings, and rebuilds its position from the last saved state plus any fills after it. Then it connects to Interactive Brokers and catches up: today's fills it may have missed, the orders still working, and finally the account's positions. These are compared, contract by contract, with what the strategies hold. A strategy may only start once they agree. Contracts that no strategy trades (a trade placed by hand, say) are listed but not compared.

**At expiry**, an option a strategy holds is settled by the engine itself, using the underlying's price at the 16:00 New York close. SPXW options settle in cash at their intrinsic value. SPY options that finish in the money deliver shares at the strike. The settlement is booked like any other fill. If the engine was not running at the close and has no price for it, the option waits, and its strategy cannot start until the operator settles it by hand with the closing price.

**Names are permanent.** A strategy's orders and fills are saved under its name, so a name is never reused for a different strategy. A removed strategy can be brought back with its position.

**Each trading morning** the engine is stopped and started again with `entry_live --sync SPY --sync SPXW --resume`. It first fetches today's option contracts from Interactive Brokers, then connects, checks the positions, and starts the market data and the strategies that were running.

The engine only trades the one account named in `IB_ACCOUNT`, which must be a paper account. A live account is refused unless the machine's administrator names it in a root-owned file. The other settings (where IB Gateway listens, how many quote lines to use, where to record quotes) come from the environment and are listed by `entry_live --help`.

## Backtests

A backtest replays one or more days of recorded quotes:

```bash
./engine/build/entry_backtest --list-strategies          # every strategy and its parameters
./engine/build/entry_backtest --files data/SPY/SPY-2025-03/*.parquet ComboStrategy leg1_ratio=1 leg2_ratio=1 leg2_right=1
```

Before a day starts, every minute's snapshot is priced in one pass, in parallel. The day then runs minute by minute: the snapshot is applied, orders sent at the previous minute are filled (buying at the ask, selling at the bid, adjusted by the slippage), and the timer runs the strategies. Each day runs on its own engine, four at a time. Options can be priced from the bid, the ask or the mid, and fees, slippage and the interest rate are options on the command line.

The program prints a single JSON document: the summary (P&L, fees, drawdown, Sharpe ratio, the largest Greeks held, the P&L by source), each day's result, and the series the page draws. Logs go to the error stream, so the output is always clean JSON. The data format is described in [data.md](data.md).

## A simulated market

The live engine can also run with no broker at all: `entry_simulate <day.parquet>` is the same engine, served the same way, but its venue, its quotes and its clock are simulated ones. It replays a recorded day (the backtest's files, real or synthetic) as if it were the live market. The engine's clock starts at the day's first quote and then runs at the wall clock's pace, so a strategy sees one minute of quotes per real minute. Orders are filled at the quotes, the same way a backtest fills them (buying at the ask, selling at the bid), and the fills build up a simulated account: cash, positions valued at the latest prices, realised and unrealised P&L, saved next to the database so the account carries on across restarts. Expiring options are settled at the close like real ones. Fifteen minutes after the last quote the engine stops, so a service can start it again on the next day.

Everything else is the real engine: the same strategies, booking, risk checks, settlement, P&L breakdown and logs, and the same API, so the page shows a simulated account exactly as it shows the paper account. The engine does not know the difference: it is built with whatever venue, source of quotes and clock it is handed, and only `entry_simulate` hands it simulated ones, as `entry_live` hands it Interactive Brokers. This is what powers the public demo; it is also a way to watch a strategy trade through a whole day without a broker.

## What Interactive Brokers sends back

The details of the broker's reports matter, because booking depends on them. Everything below was recorded on the paper account on 2026-10-12 by the integration test `IbPaper.ComboFillsAreBookedAsLegs`, which can be run again after an upgrade (during market hours; it trades and ends flat):

```bash
set -a && . ./.env && set +a && cd engine/build && OPTRA_IB_PAPER_TESTS=1 OPTRA_IB_PAPER_FILLS=1 OPTRA_IB_PAPER_RAW=1 \
  ./tests/integration/test_ib_paper_gtest --gtest_filter='*ComboFillsAreBookedAsLegs*'
```

**A single option.** IB sends the order's state several times (`PreSubmitted`, then `Filled`, often repeated with nothing new) and one execution with the side, the quantity in contracts and the price per share. The option is identified by its OCC symbol, from which the engine names it (for example `SPXW-20261013-C-7820`). The execution's timestamp is in whatever time zone IB Gateway is set to, so the engine stamps fills itself when it books them.

**A combination** (IB calls it a BAG) produces three executions for a two-leg fill, all under one order number:

```
secType=BAG  side=BOT price=2.3                       the combination as a whole: the net price per unit
secType=OPT  SPXW 261013C07815000  side=BOT price=12.4    the first leg
secType=OPT  SPXW 261013C07820000  side=SLD price=10.1    the second leg
```

The combination's own execution is not a trade of any one contract, so it is not booked as a fill; the two legs are, each at its own side and price (12.4 − 10.1 = the 2.3 paid). IB reports the order as filled *before* the legs' executions arrive, which is exactly the case the engine's booking rules exist for.

**A combination of one option gets no answer at all** from IB: no acknowledgement, no error. So the engine always sends a single option as an ordinary order.

**After reconnecting**, IB resends all of today's executions for the connection's client number, whichever process placed them. The engine drops the ones it has already booked, and logs (but does not hold) fills of orders it does not know. Because order numbers restart per client number and session, each database uses one client number.

Some informational messages are routine (2104, 2106, 2158: market data connected; 2100: account updates unsubscribed). One is worth knowing: **10197** means no market data, because the live login that the paper account shares data with is logged in somewhere else (TWS, the mobile app, the web portal).

## Speed

Most of the engine's speed comes from not doing unnecessary work on the event thread.

- **Nothing is copied to be passed along.** Events carry a pointer to their data, and the data lives in pools of reusable objects, so a quote, an order or a fill is written once by its producer and read in place by everything that needs it. Copies happen only at the edges: the network, the database and the log.
- **Nothing on the event thread waits** for the network, the disk or the log, as described above.
- **Prices are worked out once.** Live, a snapshot only reprices options whose inputs changed. In a backtest, every minute's snapshot is priced before the day runs.

These paths are measured by `scripts/bench.sh`, which times, through the real threads, how long a quote takes to become an order, a fill to reach its strategy, and a command to make a round trip.

The numerical library is parallelised with OpenMP rather than a hand-written thread pool, and a benchmark in `engine/tests/bench/concurrency` shows why. On an 8-core laptop, OpenMP was as fast as the pool or faster in every case, and up to 1.8 times faster with 16 threads, because one American-option valuation enters about a hundred parallel sections and a pool pays a queue, an allocation and a wake-up for each piece of work. Results are bit-for-bit identical whatever the number of threads: work is split into fixed chunks, each with its own random numbers, combined in a fixed order.

## Building

The engine builds on Linux with GCC 14 or newer and the Ubuntu packages listed in the README.

```bash
./build.sh r             # Release build: entry_backtest, entry_live, entry_simulate and the numerics library, into engine/build
./build.sh r 0 g         # a clean build that includes the Interactive Brokers connection
```

The pieces build as separate libraries, from the bottom up: general utilities and the numerical library; the data model; the strategy framework; the connections to the outside (IB, the backtest venue and data, SQLite); the engine itself; and the two programs on top. The IB connection needs the TWS API, which is fetched rather than kept in the repository (`scripts/fetch_ibjts.sh`; `build.sh` does it when needed).

## Testing

```bash
ctest --test-dir engine/build                  # about 150 unit tests: no network, no database file
scripts/test_sanitize.sh thread                # the threaded tests under ThreadSanitizer (or address)
```

The unit tests cover the lock-free queues and pools, the booking rules (out-of-order and repeated reports, a fill after a cancel), positions and their P&L breakdown, the risk check, settlement, restoring after a restart, shutting down (closing the engine must not stop its strategies), the IB connection against a fake broker, and the strategy framework.

Two other kinds of test run by hand. The **paper-account tests** trade on the real IB paper account, so they run only during market hours and only when asked. The **data checks** run the engine on synthetic days whose answers are known: that it reads back the implied volatility the quotes were priced at, and that a delta-hedged straddle earns what the model says it should.

Finally, every change to the engine is checked against the example strategies' backtest results from the last run, which should not move unless the change means them to.
