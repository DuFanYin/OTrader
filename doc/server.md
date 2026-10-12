# The server and the page

The server is the part of Optra you talk to. It serves the web page, answers every question the page asks, runs backtests and the research tools, and keeps a history of every computation. It is a Python web server (FastAPI) in `app/server`; the page is a small Preact app in `app/web` that the server hands out at `/`.

This page explains what the server does, how the page is organised, and how the whole thing runs day to day on the ThinkPad. The research tools have their own page, [research.md](research.md).

## Running it

```bash
cd app/web && npm ci && npm run build                          # build the page once
cd app/server && uv run uvicorn main:app --port 8080            # the server, with the page at http://localhost:8080
./scripts/dev.sh                                                # for development: engine, server and a live-reloading page
```

The server works without the live engine. Backtests and the research tools are fully usable, and the trading views say the engine is not running.

## What the server does

The server stands between the page and three things that do the real work, and it uses a different kind of connection for each (the reasons are in [architecture.md](architecture.md#why-the-engine-is-a-separate-process)):

- **The live engine**, over gRPC. Every trading view of the page, from the list of strategies to the account's positions, is a question passed on to the engine. The server keeps no trading state itself and never opens the engine's database, so it can be restarted at any time without affecting trading.
- **The backtester**, as a separate program for each run. The server finds the days asked for, starts `entry_backtest`, and returns its result. A running backtest can be cancelled from the page.
- **The numerical library**, loaded into the server itself, for the research tools.

When the engine refuses something, the page gets a clear reason with a fitting HTTP status: a request that makes no sense (400), something that does not exist (404), something the engine's current state does not allow, like removing a strategy that still holds positions (409), or the engine or IB being unreachable (503, 504).

The API is grouped the way the page is:

| Area | What it covers |
|---|---|
| The live engine | connecting to IB, market data, strategies (add, initialise, start, stop, remove, restore), positions, the account, risk limits and the halt, the log |
| The engine's records | saved orders and fills, contracts, syncing contracts from IB, clearing records |
| Backtests | the recorded days available, running and cancelling a backtest, making synthetic or recorded days |
| The trading day | the health check, the end-of-day report, comparing a paper day with its backtest |
| Research | every research tool, and the history of runs |

Every route, with its inputs and outputs, is listed at `/docs` on the running server.

## The page

The page has one top bar, showing whether the engine is running and connected to IB and to market data, and two groups of tabs.

**Trading** is about the live engine:

- **Home** gives the day at a glance: the account's value and P&L, how many strategies are running, open positions, today's fills, refused orders, and whether the health check is happy. Below that are the strategies themselves (start them, stop them, see what each holds), the latest fills, the risk limits, and a stress test of everything held.
- **Account** shows the IB account's values and positions next to what the strategies think they hold, contract by contract, and is where expired options are settled or a missed fill is entered by hand.
- **Orders & trades** lists every order and fill the engine saved.
- **Data** shows which days can be backtested and which contracts the engine knows, and syncs new contracts from IB.

**Research** holds the backtester and the research tools: Backtest, Screener, Pricing, Multi-Leg, Risk, Numerics and Validation. They are described in [research.md](research.md#the-research-views).

Every view is laid out the same way: the inputs on the left with one main button (Ctrl+Enter or ⌘↵ runs it), the results on the right. A few rules keep it pleasant to use:

- **The sidebar stays short.** Only the inputs that decide a run are there. Groups of detailed settings, like a strategy's parameters or Monte Carlo settings, show a one-line summary and open in a dialog.
- **The results side is never empty.** A research view opens with a sample result, a run made with the default inputs and clearly marked "Sample" until you run it yourself. All views load their data when the page opens, so switching tabs shows results immediately.
- **Numbers hold still.** Live values are shown with a fixed number of decimals, and live market data is refreshed when you ask or when it is more than a minute old, not on every tab switch.
- **History and log are one click away**, in drawers at the bottom right: every research run you made (reload it, or compare two), and the engine's log as it happens.

The page talks only to the server, and only runs its own scripts.

## Running it day to day

On the ThinkPad, Optra runs as a few systemd services and timers (their files are in `~/repo/dufanyin/deploy`):

| Service or timer | Does |
|---|---|
| the engine | trades on the IB paper account all day |
| the server | serves the page and the API on port 8080 |
| the morning restart, 09:00 New York | restarts the engine with today's contracts; it resumes where it stopped |
| the health check, every 5 minutes during the session | asks whether the engine is up, connected, streaming, agreeing with IB and not halted, and sends a Telegram message when that changes |
| the report, 16:20 New York | writes the day's report: fills and P&L by strategy, refusals, the position check, and each strategy's paper day replayed as a backtest; a summary goes to Telegram |
| the alert | a Telegram message if any of the above fails |

The page is reachable at `dufanyin.dev/optra/`, for the signed-in owner only (or from the tailnet). The engine itself only listens on the machine.

After changing the code: rebuild the engine with `./build.sh r` and restart the engine service (only when it is fine for trading to pause); restart the server service for server changes; run `npm run build` in `app/web` for page changes. None of these need sudo.

## The public demo

A second copy of Optra runs as a public demo at `dufanyin.dev/optra-demo/`. Its engine trades a simulated account on synthetic SPY days (see [the engine](engine.md#a-simulated-market)), and its server runs with `OPTRA_DEMO=1`, which adds a Demo badge saying where the data comes from to the page's header and names the venue Sim (its connection Sim gateway) where the page would say IB; otherwise the page is the same. Visitors can do what the owner can, from adding and starting strategies to running backtests and the research tools; only what would wipe or restart the demo for everyone else is refused, with a pop-up. The demo is deployed from a frozen copy kept in the `dufanyin` repository, so changes here reach it only when that copy is refreshed.

## Tests

From `app/server`, `uv run --group dev --group databento pytest -q` runs everything in about 15 minutes, because the trading tests run real backtests. `tests/tools` alone runs offline in about a minute. Each test run gets its own temporary home folder, so tests never touch the real run history.
