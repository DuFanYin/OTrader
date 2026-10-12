# How Optra fits together

Optra is a system for trading options with rules. You write a strategy once, in C++. The same code runs a backtest on days of recorded option quotes, and runs live on an Interactive Brokers paper account. Around it sit research tools for finding trades worth trying: pricing, calibration, stress tests, a screener that ranks option combinations. A web page puts all of this in front of you, and the same functions are open to an AI agent through an HTTP API.

This page explains how the pieces fit and why they are split the way they are. Each piece then has its own page: [the engine](engine.md), [the server and the page](server.md), [the research tools](research.md), [backtest data](data.md).

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="images/architecture-dark.png">
  <img width="720" alt="Optra's architecture: the page and the server above, the engine and its connections to the outside below" src="images/architecture-light.png">
</picture>

## Three programs

Optra is three programs that run side by side.

**The engine** does the trading. It is written in C++ because it has to be fast and predictable. When it trades live, it is one long-running process (`entry_live`) that talks to Interactive Brokers, keeps every order and fill in a small SQLite database, and runs your strategies. When it backtests, it is a short-lived program (`entry_backtest`) that replays a range of recorded days and prints the result.

**The server** is a Python web server (FastAPI). It gives the page everything the page shows. It also runs the research tools and keeps a history of every computation. The server can be restarted at any moment without touching a single order, because it holds no trading state of its own.

**The page** is what you look at in the browser. It has two groups of tabs. *Trading* shows the live engine: its strategies, the account, orders, data. *Research* holds the backtester and the research tools.

Underneath both the engine and the research tools is one numerical library: Black-Scholes, implied volatility, Monte Carlo, trees, finite differences, Heston. A price or a Greek is computed the same way wherever you see it.

## Why the engine is a separate process

The engine owns money, the server does not. Live, the engine has to keep running through anything the server does: an update, a crash, a restart. So the two only talk over a narrow, explicit interface (gRPC on localhost). The server asks for what it needs ("list the strategies", "start this one"), and the engine answers. The server never reads the engine's database behind its back.

Backtests are different again. They are run as a separate program for each request, so they can run in parallel, can be cancelled by simply stopping the process, and can never disturb the live engine or the server's memory. The program's output is a single JSON document with the result.

The research tools only compute. They are stateless: numbers in, numbers out. So the server loads the numerical library directly into its own process and calls it like a function.

That gives three kinds of connection, each for one kind of work:

| To | How | Because |
|---|---|---|
| the live engine | gRPC | it holds the trading state and must outlive the server |
| a backtest | a separate process | parallel, cancellable, isolated |
| the numerics | a library loaded into the server | stateless, and fast to call |

## What happens when a quote arrives

Following one quote through the live engine shows most of its design.

1. Interactive Brokers sends a new bid and ask for an SPXW option. The engine's market-data thread writes it into its quote book.
2. After each batch of quotes, that thread rebuilds the portfolio's *snapshot*: every option's price, its expiry's forward price (worked out from put-call parity), its implied volatility and its Greeks. Only the options whose inputs changed are priced again.
3. The snapshot is handed to the engine's single *event thread*. If an older snapshot is still waiting, the new one simply replaces it: there is no point acting on stale prices.
4. The event thread applies the snapshot and calls each strategy trading that portfolio. A strategy may decide to send an order.
5. The order goes through the risk check (size limits, a price band, no short options, and so on) and then to the IB connection, which writes it to the network on its own thread. The event thread does not wait for the network.
6. When IB reports a fill, the fill goes onto an ordered queue that never drops anything, and the event thread books it: into the order, into the strategy's position, into the database. Then the strategy hears about it.

Two ideas carry this design. First, **one thread owns all trading state**. Every change to orders and positions happens on the event thread, one after another, so there are no locks to get wrong and the order of events is the same as in a backtest. Second, **the event thread never waits on the outside world**. Logging, network writes and database writes all happen on other threads.

## The same code in a backtest

In a backtest, the engine is the same object with different connections plugged in. Instead of a live market feed, the quotes come from a day's file of recorded data. Instead of IB, a simulated venue fills orders at the next bar's quotes (buying at the ask, selling at the bid, with fees and slippage). Instead of a database file, an in-memory one. And instead of a background event thread, each event is handled the moment it is produced.

Because the strategy code, the booking, the risk check and the settlement of expiring options are all written once, a strategy that works in the backtest behaves the same way live. Several days run in parallel, each on its own engine.

## Restarting without losing anything

The live engine is stopped and started again every trading morning, and it may restart at other times too. It always picks up where it left off. Every order and fill is saved. On start, the engine rebuilds each strategy's position from what it saved, asks IB for today's fills and working orders to catch anything it missed, and compares its positions with the account's, contract by contract. A strategy is only allowed to start once the two agree. Stopping the engine does not stop the strategies: they are simply resumed on the next start, positions included.

## From an idea to the paper account

The research tools and the engine are designed to meet. Here is the path a trade idea takes:

1. **Screen.** The screener ranks straddles, strangles, iron condors and calendars on a chain of options, by their "edge" against a model's value. The chain can be live (BTC or ETH from Deribit) or a minute of a recorded SPXW or SPY day.
2. **Turn it into a rule.** A picked combination becomes a rule the engine can follow every day: "buy a straddle at the money, expiring today, at 10:00, and close it at 15:50, or earlier at twice the premium." Strikes are given relative to the forward price, so the rule works on any day.
3. **Backtest it.** The rule runs over every recorded day. The profit and loss is split into what came from the underlying's moves (delta and gamma), from time passing (theta), from volatility (vega), and from the prices paid at the fills.
4. **Check the hedge.** The same rule runs again with the engine's delta hedge on, to see what hedging changes.
5. **Trade it on paper.** The same settings go to the paper account, where the engine computes the same Greeks and the same breakdown live.

## Who can reach what

- The engine trades **one paper account** and refuses a live one, unless the machine's administrator explicitly names it in a root-owned file.
- **Every order passes the risk check**, live and in backtests alike, and the operator can halt all trading except orders that only close positions.
- The page and its API are only reachable by the signed-in owner, at `dufanyin.dev/optra/`. The engine's own interface listens only on the machine itself.

## Where things are

| Folder | Holds |
|---|---|
| `engine/` | the C++ engine, its numerical library and its tests |
| `app/server/` | the server: the API, the research tools, the run history |
| `app/web/` | the page |
| `scripts/` | build and development tools, and the command lines that prepare backtest data |
| `data/` | backtest days, recordings and the live database (not in the repository) |
| `doc/` | these pages |

For working on the code, [AGENTS.md](../AGENTS.md) lists the commands, conventions and rules.
