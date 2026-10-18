# How Optra fits together

Optra trades options with rules. A strategy is written once, in C++; the same code backtests on recorded option quotes and trades live on an Interactive Brokers paper account. Research tools (pricing, calibration, stress, a screener) sit beside it, on one web page and one HTTP API that an AI agent can use too.

Each part has its own page: [the engine](engine.md), [its latency](latency.md), [the server and the page](server.md), [the research tools](research.md), [backtest data](data.md), [backtesting](backtesting.md).

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="images/architecture-dark.png">
  <img width="720" alt="Optra's architecture: the page and the server above, the engine and its connections to the outside below" src="images/architecture-light.png">
</picture>

## The programs

| Program | What it is | State it holds |
|---|---|---|
| `entry_live` | the engine trading live: one long-running C++ process, connected to IB | orders, fills, strategies, in `data/optra.db` (SQLite) |
| `entry_backtest` | the engine on recorded days: a short-lived process, one JSON document out | none after it exits |
| `entry_simulate` | the live engine with a recorded day as its market, for the public demo | a simulated account |
| the server (`app/server`) | Python (FastAPI): the page's API, the research tools, the run history, the job queue | no trading state; restarts at any time |
| the research workers | processes that take backtest jobs from the queue, each with its own memory limit | none |
| the page (`app/web`) | the browser UI: *Trading* (the live engine) and *Research* (backtests and tools) | none |

One numerical library (`engine/quant`) prices for the engine and the research tools alike: Black-Scholes, implied volatility, Monte Carlo, trees, finite differences, Heston.

## Why the engine is a separate process

The live engine holds the orders and positions; the server holds none. The engine keeps running when the server is updated, crashes or restarts, so the two connect only through the interfaces below.

| From the server to | How | Why |
|---|---|---|
| the live engine | gRPC on localhost | the engine holds the trading state and outlives the server; the server never reads its database |
| a backtest | a job in the queue, run by a worker as `entry_backtest` | by priority, cancellable, a crash or out-of-memory stays in the worker |
| the numerics | the C library in a pool of two processes | stateless, fast to call; a crash or hang costs one calculation |

## A quote through the live engine

1. IB sends a bid and ask. The market-data thread writes it into its quote book.
2. After each batch, that thread prices the portfolio's snapshot: forwards from put-call parity, implied volatilities, Greeks. Only what changed is priced again.
3. The snapshot goes into the portfolio's slot for the event thread; a newer one replaces one not yet applied.
4. The event thread applies it and calls the strategies on that portfolio. A strategy may send an order.
5. The order passes the risk check, then goes to the IB connection, which writes it to the network on its own thread.
6. A fill comes back through an ordered queue that never drops anything. The event thread books it (order, position, database), then tells the strategy.

Two rules:

- **One thread owns all trading state.** Orders and positions change only on the event thread, one event after another: no locks, and the same order of events as in a backtest.
- **The event thread never waits on the outside world.** Network, log and database writes are on other threads.

## The same code in a backtest

| Part | Live | Backtest |
|---|---|---|
| Quotes | IB, on the market-data thread | a day's file, a minute at a time |
| Venue | IB | a simulated venue: fills at the next minute's quotes, at a set position in the spread, with fees |
| Records | SQLite | in memory |
| Event loop | its own thread | events handled as they are put |
| Strategies, booking, risk, settlement | the same code | the same code |

Many settings of a strategy run side by side, each on its own engine, every day read and priced once for all of them ([backtesting.md](backtesting.md)).

## Restarts

- The live engine restarts every trading morning and may restart at other times.
- Every order and fill is saved. On start, each strategy's position is rebuilt from them, IB is asked for the day's fills and working orders, and the account's positions are compared with the strategies', contract by contract. A strategy starts only once they agree.
- Stopping the engine does not stop the strategies: they resume on the next start, positions included.

## From an idea to the paper account

1. **Screen**: rank straddles, strangles, iron condors and calendars on a chain by edge against a model's value (live BTC or ETH from Deribit, or a minute of a recorded SPXW or SPY day).
2. **Make it a rule**: legs with strikes relative to the forward and days to expiry, entry and exit minutes, profit and loss exits (`ComboStrategy`).
3. **Backtest it** over every recorded day; P&L split into delta, gamma, theta, vega and the cost of each fill.
4. **Check the hedge**: the same rule with the engine's delta hedge on.
5. **Trade it on paper** with the same settings; the same Greeks and P&L breakdown live.

## Who can reach what

- The engine trades **one paper account** and refuses a live one unless the machine's administrator names it in a root-owned file.
- **Every order passes the risk check**, live and in backtests. The operator can halt all trading but closing orders.
- The page and its API are reachable only by the signed-in owner, at `dufanyin.dev/optra/`. The engine's gRPC listens only on the machine.

## Where things are

| Folder | Holds |
|---|---|
| `engine/` | the C++ engine, the numerical library, the programs, the tests |
| `strategy/demo/` | the example strategies (other strategy folders are local) |
| `app/server/` | the server: API, research tools, run history, job queue |
| `app/web/` | the page |
| `scripts/` | build and development tools, data command lines |
| `data/` | backtest days, recordings, the live database (not in the repository) |
| `doc/` | these pages |

For working on the code, [AGENTS.md](../AGENTS.md) lists the commands, conventions and rules.
