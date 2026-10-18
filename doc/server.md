# The server and the page

The server is the part of Optra you talk to. It serves the web page, answers every question the page asks, runs backtests and the research tools, and keeps a history of every computation. It is a Python web server (FastAPI) in `app/server`; the page is a small Preact app in `app/web` that the server hands out at `/`. The research tools themselves are described in [research.md](research.md).

## Running it

```bash
cd app/web && npm ci && npm run build                          # build the page once
cd app/server && uv run uvicorn main:app --port 8080            # the server, with the page at http://localhost:8080
./scripts/dev.sh                                                # development: engine, server and a live-reloading page
```

The server works without the live engine: backtests and the research tools are fully usable, and the trading views say the engine is not running.

## What the server does

The server stands between the page and three things that do the real work, each over its own kind of connection (why is in [architecture.md](architecture.md#why-the-engine-is-a-separate-process)):

- **The trading engines**, over gRPC, one for each account: the paper account's (`LIVE_GRPC_TARGET`) and, once there is one, the live account's (`LIVE_ACCOUNT_GRPC_TARGET`). Every trading view is a question passed on to an engine: the paper account's, or the live one's with `?account=live`; the health check and the end-of-day report take the same. The server keeps no trading state and never opens an engine's database, so it can be restarted at any time without affecting trading. Every call has a deadline, so an engine that stops answering costs the page an error message, never the server.
- **The research workers**, through a queue. The server never runs a backtest itself. Every backtest it needs is a job that a worker runs with `entry_backtest`; a running one can be cancelled from the page.
- **The numerical library**, in two small processes of its own, so a crash or a hang there costs one calculation.

When the engine refuses something, the page gets the reason with a fitting status: 400, 404, 409 (not allowed in the engine's state, like removing a strategy that holds positions), or 503 and 504 when the engine or IB cannot be reached.

The API is grouped as the page is:

| Area | What it covers |
|---|---|
| The live engine | connecting to IB, market data, strategies, positions, the account, risk limits and the halt, the log |
| The engine's records | saved orders and fills, contracts, syncing contracts from IB |
| Backtests | the days available, running and cancelling backtests, making synthetic days |
| Experiments | trials registered, counted and run in batches; the statistics on their results |
| The trading day | the health check, the end-of-day report, a paper day against its backtest |
| Research | every research tool, and the history of runs |

Every route is listed with its inputs and outputs at `/docs` on the running server.

## The page

A top bar shows whether the engine is running and connected to IB and to market data. Below it are two groups of tabs.

**Trading** is the live engine:

- **Home**: the account's value and P&L, the strategies (start, stop, what each holds), today's fills and refusals, the risk limits, a stress test of everything held, and a strategy of your own: its C++ pasted, built and installed for the paper account (read only in the public demo). The strategies, their holdings and the risk state come pushed from the server as they change (`GET /api/live/stream`, Server-Sent Events: one reader of the engine for every page watching), polled every 3 seconds only where the stream cannot be had.
- **Account**: the IB account's positions next to what the strategies hold, contract by contract; where expired options are settled and a missed fill is entered by hand.
- **Orders & fills**: every order and fill the engine saved.
- **Data**: which days can be backtested, which contracts the engine knows, and syncing new ones from IB.

**Research** is where agents do most of the work:

- **Experiments**: each experiment with its trials ranked, and each family with how much of its budget it has used.
- **Backtest**: one run in detail, its P&L and Greeks minute by minute and each day.
- **Lab**: the calculators the agents' tools are made of (screener, pricing, multi-leg, risk, numerics, validation), one at a time.
- **Tools**: every tool an agent can call, with its route, its cost and an example input.

Every view puts its inputs on the left with one main button (Ctrl+Enter or ⌘↵) and its results on the right; detailed settings open in a dialog. A research view opens with a sample result until you run your own. Drawers at the bottom right hold the run history and the engine's log. The page talks only to the server and runs only its own scripts.

## Experiments

A research idea is rarely one backtest. Trying many settings and keeping the best is exactly how a backtest comes to look better than the strategy is, so the server counts every try.

An **experiment** is a strategy, a space of its parameters, the days and the costs, under a **family**: the hypothesis all its variants test. It expands into **trials**, one setting each. A trial is registered before it runs and counts from then on, whatever happens to it. A family has a budget of trials, 200 unless raised, and an experiment that would pass it is refused whole. Results are deflated by every trial the ledger has registered, in any family, so a new family name does not reset the count.

A trial's id is a hash of everything that decides its result, its code included (hashes the build writes of every file the compiler read), so a trial already run is reused, and changing one strategy makes only its trials new.

Days on or after a portfolio's holdout start (set in `$OPTRA_HOME/config.json`) cannot be in an experiment. An agent sees them only through a version's holdout report, which the server runs itself, once per version.

Trials run as jobs in the same queue as every other backtest. A **worker** takes the most urgent job, up to 32 trials over the experiment's days, and runs it with `entry_backtest --batch`. It holds the job on a lease, so a job whose worker dies goes back to the queue, and fails after a second try. On the ThinkPad there are two workers: one for what someone is waiting for, one for anything.

Each trial's results are kept day by day in `data/experiments.db`. From the matrix of trials by days, with every try counted, the server computes:

- the **deflated Sharpe ratio** of the best trial: the chance its Sharpe ratio beats the best that as many tries without skill would show;
- the **probability of backtest overfitting**: how the best trial in sample ranks out of sample, over every way of splitting the days;
- a **walk-forward**: the best trial on each window, traded on the next;
- the best trial without its five best days, and on up, down, quiet and wild days.

Doubled costs and parameters moved 20% either way are run as experiments of their own under the same family.

For agents, `backtest` does all of this in one call: it registers a setting or a space, runs it beside a random-buying control, and answers with the measures, the statistics, the robustness checks and which review gates are met. `replay_trial` runs a trial again, uncounted, and returns its fills, its P&L by source and what it logged on the days asked for.

## From a result to trading

A result worth trading becomes a **strategy version**: a strategy, its setting frozen, its code, and a card stating the hypothesis, where the edge comes from, and what would mean it is gone. A version moves through research, review, paper, pilot and live, one stage at a time, and can be retired from any of them. Each step has gates checked against the ledger:

| To | Gate |
|---|---|
| review | deflated Sharpe above 0.95, overfitting probability below 0.2, 300 trades on 300 different days, still profitable with costs doubled, with fills held to the size shown a minute late, and with each parameter moved 20% either way |
| paper | its holdout report made money |
| pilot | 20 paper days matching a backtest of each day, paper fills paying at most 1.5 times over the mids what the backtest's did, and the paper P&L inside what its research days predict |
| live | eight weeks of pilot, still inside the prediction |

The stages that trade also need the owner's approval. A version on paper is compared each day with a backtest of that day, and watched against the band its research days predict: one whose last 20 paper days fall below it goes back a stage on its own, and the share of versions inside their prediction (the realization rate) is in the daily report. A family may look at its holdout three times; past that the holdout would become one more validation set.

A move between stages is written down first and then carried out step by step (the code installed for the account, the strategy started or stopped on the paper engine, the stage), each step safe to repeat: if one fails, the move stays under way, says why, and the five-minute check carries it out again. The move to the pilot also gives the line the machine's administrator adds to the live account's list of approved strategies: that engine runs nothing else ([engine.md](engine.md#the-live-engine)).

The server draws the line between the agent and the owner ([`infra/access.py`](../app/server/optra/infra/access.py)). The agent can ask for review; the owner approves anything that trades, raises budgets, installs strategies and works the engine. The owner's requests come through dufanyin.dev/optra/ with a passkey, which adds a key only the owner's Unix user can read; any other request is the agent's. Every route and tool says whose it is, or the server refuses to start.

An agent writes strategies through tools too: `put_strategy` writes a file into `strategy/research` and builds it, answering with the compiler's errors or the parameters it registered. Research strategies are loaded by backtests only; a version's code reaches `strategy/paper` or `strategy/live` only when approved.

## Running it day to day

On the ThinkPad, Optra runs as systemd services and timers (their unit files are kept with the machine's other deployment files, outside this repository). Only IB Gateway runs all the time, logged in; the rest (the engine, the server, the workers, the report) are one group started and stopped together as they are needed, so nothing of it must stay up: the engine picks up from its database, and a backtest is a program each job starts.

| Service or timer | Does |
|---|---|
| the engine | trades on the IB paper account, rolling in each day's contracts at 08:45 New York ([engine.md](engine.md#restarts-expiry-and-the-trading-day)) |
| the server | serves the page and the API on port 8080; restarted if it stops answering for 30 seconds |
| two research workers | run every backtest, held together to 7 GB; the first the kernel stops when memory runs short, never the engine |
| the health check, every 5 minutes in the session | checks the engine is up, connected, streaming, agreeing with IB and not halted; sends a Telegram message when that changes |
| the report, 16:20 New York | the day's fills and P&L by strategy, refusals, the position check, and each paper day replayed as a backtest; a summary goes to Telegram |
| the alert | a Telegram message if any of the above fails |

The page is at `dufanyin.dev/optra/`, for the signed-in owner only; the engine listens only on the machine. After changing code: `./build.sh r` and restart the engine service (only when trading may pause), restart the server service for server changes, and `npm run build` in `app/web` for the page. None of this needs sudo.

## The public demo

A second copy runs at `dufanyin.dev/optra-demo/`. Its engine trades a simulated account on synthetic SPY days ([engine.md](engine.md#a-simulated-market)), and its server runs with `OPTRA_DEMO=1`, which badges the page and names the venue Sim. Visitors can do what the owner can, except what would wipe or restart the demo for everyone. The demo is a frozen copy kept in the `dufanyin` repository, so changes here reach it only when that copy is refreshed.

## Tests

From `app/server`, `uv run --group dev pytest -q` runs everything in about 15 minutes, because the trading tests run real backtests; `tests/tools` alone runs offline in about a minute. Each test run gets its own temporary home folder, so tests never touch the real history.
