# The engine

The part of Optra that trades: C++23, the same code for backtests and for live trading on Interactive Brokers. How it fits with the rest: [architecture.md](architecture.md).

## Events in, orders out

A strategy does no I/O. It is called on these events:

| Event | When |
|---|---|
| snapshot | a portfolio's latest option and underlying quotes, priced (IVs; Greeks on demand) |
| timer | once a bar in a backtest, once a second live |
| order update | accepted, filled, cancelled, rejected |
| fill | a quantity of one contract at a price |

It can send orders and cancels and write log lines.

- A timer tick: settle what expired, close what would settle in shares, run the strategies, value every holding and measure its risk, check the day's loss, run the delta hedges (`core::Main::on_timer`).
- An order update or a fill is booked first, then told to its strategy.

What differs between modes is only what is plugged in:

| Plugged in | Backtest | Live |
|---|---|---|
| Quotes | a day's file of recorded quotes | IB |
| Clock | the current bar's time | the wall clock |
| Venue | a simulated venue, fills at the next bar | IB |
| Records | memory (`BacktestDatabase`) | `data/optra.db` (SQLite) |
| Event loop | events handled as they are put | its own thread |

## How the engine is organised

| Folder | What it is |
|---|---|
| `core/main.*` | `core::Main`: owns the engines and the ports, routes every event in a fixed order |
| `core/event_loop.*` | the event thread: one ordered queue, a snapshot slot per portfolio |
| `core/bookkeeping/` | each strategy's orders, fills and holding; valuation each bar; a position's life: settlement at expiry, the closes asked for when the engine missed one, closing before a share-settled expiry or an ex-dividend day; the check against the broker |
| `core/risk/` | the limits (no opening inside the window before a share-settled expiry), the exposure marks, the daily loss, the halt |
| `core/strategy/` | the strategies, their states, callback timing, the delta hedges they ask for; what they want streamed (the chains they subscribe, the options held or ordered today), one set that live market data plans from and a backtest's day loads |
| `core/market_data/` | live only: streams what the strategies want and turns the quotes into priced snapshots on its own thread |
| `core/log/` | the log thread |
| `core/gateway/`, `core/database/` | the ports: `core::Gateway`, `core::MarketData`, `core::Database` |
| `assembly/` | `Main` put together: live (`live.*`, with the trading days) and backtest (`backtest.*`: the replay, a day, many trials) |
| `adapters/` | the ports' implementations: IB, the backtest venue, the simulated market, the Parquet days, SQLite |
| `pricing/` | a snapshot's quotes made into forwards, IVs and Greeks, then each chain's smile (an SVI fitted to its IVs): the same code for live market data and the backtest's days |
| `model/` | contracts, orders, fills, the portfolio (`Listing`, `Portfolio`, `PortfolioSnapshot`), what each portfolio is (`portfolio_spec.hpp`) and its calendar (`session_calendar.hpp`: trading days, hours, roll, expiry), a strategy's record, `Param` |
| `strategy/` | what a strategy is written against: `Strategy`, `StrategyContext`, the registry |
| `quant/` | the numerical library and its C ABI ([research.md](research.md#the-methods)) |
| `entry/` | the programs and the gRPC API (`optra_engine.proto`) |

Rules:

- The engines that keep trading state do no I/O and never call anything above them, so each can be tested alone and a backtest reproduces a run to the byte.
- `Main` routes a fill to bookkeeping, then storage, then the strategy. Every order (strategy, hedge, close before expiry) goes risk check → venue → bookkeeping. The operator's commands arrive at the same place.
- Broker, storage, market data and clock are ports; a new broker is one adapter, not a change to the engine.
- Everything that changes trading state runs on the **event thread**. Other threads hand it work through lock-free queues. A command from another thread runs there and returns when done; a read is handed there too, so nothing outside sees half-changed state.

## Writing a strategy

- A C++ class with callbacks: init, each trading day (`on_day`), each snapshot, each timer tick, its order updates and fills.
- The base class gives the trading day and minute, chain and strike selection, its holding, and order sending. Time is read with `now()`, never by counting ticks.
- Parameters are a struct of `Param` lines (name, default, range) that the page can show and check; the risk limits are declared the same way. The setting is checked once when the strategy is added.
- A strategy sees the engine only through its `StrategyContext`:

| It reads | It does |
|---|---|
| its record (name, class, portfolio, setting) | `send_order` |
| its portfolio: quotes, IVs and Greeks, each chain's forward and smile (`Chain::smile`, `smile_iv`, `smile_iv_at_delta`) | `register_hedging` / `unregister_hedging` |
| its holding, whether an order is working | `subscribe_chains` |
| time to an expiry: calendar years in every IV (365 days, as Cboe's VIX), or `variance_years` on a variance clock (each half hour's share of a session's variance, a night before each open) | |
| values by day (`daily()`, from `data/daily`) | `write_log`, `set_error`; `write_value`: a value of the day, reported with the backtest's days (what a strategy that only measures answers with) |

- It cannot reach the database, the network or other strategies.
- Heavy computation does not belong in a callback: it runs on the event thread, which every portfolio and fill waits on. What is computed per chain (forwards, smiles) arrives with the snapshot, computed on the market-data thread live and once per day in a backtest.
- States: added → inited → running, or error. Each step only from the one before; only a running strategy can be stopped, so a stop never closes positions by mistake. A callback that throws, or keeps running over its time budget, puts it in error until it is inited again.
- Each `.cpp` under `strategy/` is its own library, loaded at run time. Backtests load the demo and research strategies; a trading engine loads the demo ones and those installed for its account, which takes a person. Agent-written code stays off a trading account.

**ComboStrategy** (`strategy/demo/`) trades one combination described by rules: up to four legs, each with ratio, call or put, strike relative to the forward, days to expiry. It opens and closes at set minutes, exits on profit or loss, can delta-hedge, and can wait for an edge over each leg's chain's fitted smile. It runs a combo picked in the lab and sweeps a structure; a new idea is a class of its own in `strategy/research/`.

**SurfaceStrategy** (`strategy/demo/`) trades nothing: each day, 15 minutes before the close, it measures the volatility surface (ATM IV at 7, 30 and 90 days, the 25 delta risk reversal and butterfly) for the values by day ([data.md](data.md#values-by-day)).

## Booking orders and fills

| Rule | What it means |
|---|---|
| An order is units of legs | each leg a contract, a side and a ratio; one contract is one leg. A limit price is the net per unit, as IB prices a combination. Only the IB adapter treats combinations specially |
| A position is one contract's | each leg is booked to the strategy's position in that contract, so holdings are checked against the broker contract by contract; each position remembers the combination it was opened in |
| The order of reports does not matter | updates merged, repeats dropped, "done" held back until its fills are booked; an order stays active until the broker ends it, even after a cancel |
| P&L is explained | each bar, split into delta, gamma, theta, vega, each fill's cost against the mid, and the rest; the parts add up to the total |

Fees follow IB: 1.2 USD an option contract by default in research; 0.005 USD a share, 1 USD minimum an order.

## The risk check

Every order passes it, in backtests as well as live (`core/risk/risk_engine.hpp` lists every limit):

| Kind | Limits |
|---|---|
| Size | contracts or shares in one order; what a strategy may hold of a contract; options held at once |
| Price | premium paid; a limit price's distance from the mid; the spread a market order may cross |
| Shape | options sold uncovered refused when `short_options` is 0; SPY options expiring today closed `flatten_minutes` before the close, none bought after |
| Exposure | delta, gamma and vega in USD, per strategy and in total; margin |
| Loss | a strategy past its daily loss only closes until the next day; all together past theirs halts trading to closing orders |
| Data | no opening order on quotes older than `max_quote_age_seconds`, nor while the broker is disconnected |

- Exposure, margin and the loss since the New York day began are marked every bar. On a day the market is closed only the marks move.
- Margin, estimated: per expiry, the most the options can lose at expiry (nothing for options bought), plus 20% of the underlying for each call sold uncovered; shares, half their value.
- The operator can halt to closing orders at any time.

## The live engine

One process all day. The event thread runs the strategies and books everything; IB, market data, the log, the database and the gRPC requests have threads of their own ([latency.md](latency.md)).

| Layer | Start | Stop |
|---|---|---|
| The engine | log, database, event thread; strategies, orders and fills restored | the event thread drains its queue; the database closes |
| The run | connect to IB, roll in the day's contracts, start market data and the strategies that were running | stop the daily roll, then market data, then IB |

- Strategies are not stopped when the engine stops (a stop may close positions); they resume on the next start.
- If IB cannot be reached or the day's contracts cannot be fetched, nothing trades and the engine tries again each minute, logging the failure once.
- Accounts: only `IB_ACCOUNT`, a paper account. A live account must be named by the administrator in `/etc/optra/live-account`, and then runs only the strategies listed in `/etc/optra/live-strategies`, a line each: class, the code its library was built from, its setting (`ComboStrategy e20212171aaeb093 entry_minute=30.0 ...`; what the line leaves out is the default). Anything else is refused by the engine itself.
- Other settings: `entry_live --help`.

## Restarts, expiry and the trading day

- **Restart**: every order, fill and setting is saved as it happens. On start each strategy comes back under its name, its holding replayed from its orders and fills; the engine catches up with IB and compares positions contract by contract; no strategy starts until they agree.
- **A strategy restarted mid-day** starts with its members empty and reads what it did today from its holding (`traded_today`, `last_order_minute`, `holds_options`). A backtest trial's `restart_minute` makes the strategy again at that minute each day to check this.
- **The engine started after the open**: the underlying's open, high and low of the day (`Portfolio::day_open`, `day_high`, `day_low`) are asked of the source once its first quote comes after the first five minutes: IB's one-minute bars of the day (midpoints; an index's trades), the open being the first minute's close as a backtest's first bar. Until the answer, `Strategy::saw_open` is false.
- **Expiry**: at the New York close the engine settles expiring options itself: SPXW and XSP in cash at intrinsic value, SPY, QQQ and IWM in the money by delivering shares at the strike (`model/portfolio_spec.hpp`). If the engine was down at the close, it asks the broker for that day's official close and settles at it when the answer comes; the strategy cannot start until then (the operator can settle by hand).
- **Dividends**: the day before an ex-dividend day of a share-settled underlying, combos with a call sold are closed before the close (early exercise). The ex-dividend days are read from `data/daily/<underlying>_ex_dividend.csv`; without that file the rule does not apply.
- **Names**: orders and fills are saved under the strategy's name, so a name is never reused and a removed strategy can be restored with its holding.
- **The daily roll**: at 08:45 New York the day's contracts are fetched and rolled in without stopping. Held contracts stay, expired ones go; strategies pick the day's chains in `on_day`. Option and chain handles are good only until the next roll, so a strategy keeps symbols.

## Backtests

```bash
./engine/build/entry_backtest --list-strategies          # every strategy and its parameters
./engine/build/entry_backtest --files data/backtest/SPY/SPY-2025-03/*.parquet ComboStrategy leg1_ratio=1 leg2_ratio=1 leg2_right=1 \
    --costs '{"fee_scale": 1.0, "multi_leg_fill_position": 0.5}'
./engine/build/entry_backtest --batch spec.json                # many trials over the same days
```

- Days run as live trades them: one engine and one strategy, in date order, each morning's contracts rolled in from the day's file.
- **Fills**: each minute, orders from the minute before fill at a set position in each leg's spread (ask or bid by default; one position for single-leg orders, another for combos), optionally limited to the size shown or delayed. These costs are one object defined in the server (a scale on the fees, the two positions, the fill model, the delay, the risk limits) and handed to the engine whole. The fees themselves are the engine's: IBKR's commission by an option's price and an order's least (`BacktestGateway`), and each portfolio's exchange fee (`model::PortfolioSpec`, SPXW's Cboe index fee); a new exchange or root is a line there.
- **At the close**: expiring options settle, working orders are cancelled, holdings carry to the next day.
- **Batch**: many trials over the same days, each on its own engine, each day read and priced once for all ([backtesting.md](backtesting.md)). A trial gives exactly the result it gives alone.
- **Output**: for a person, text: the run, its setting and costs, each day's net P&L, fees and orders, the round trips, the drawdown and the P&L by source (a run from the command line is charged IBKR's fees unless `--costs` says otherwise). With `--json` (and always with `--batch`), one JSON line per trial (its days and round trips as columns), then a line with the days (each file, its bars and quotes, the underlying's open and close); fills only when asked. The server reads only the JSON and sums the days, in one place, for the ledger, the tools and the page.
- **Day cache**: a day's contracts and minutes in a `.day` file, each chain loaded in a `.chain` file (quotes, forwards, IVs, each minute's smile), read in place. A chain is loaded only when a strategy subscribes to it, holds or orders one of its options. Greeks are not stored: they are priced on demand by the live code, bit for bit the same.
- **Round trips**: from the bar a position opens to the bar it holds nothing again, the same day or later (an option exercised ends when its delivered shares are sold the next morning). Each has its P&L after fees and by source, its maximum loss at the open, what its orders paid over the mids, and what its options settled for. Per-trade measures (R, win rate, number of trades) are built from these.
- **Errors**: a run stops rather than give a wrong answer, for example when a day is missing between two held days. Logs go to stderr. The data format is in [data.md](data.md).

## A simulated market

`entry_simulate <day.parquet>` runs the live engine with no broker, replaying a recorded day as the market at wall-clock pace. Orders fill as in a backtest and build a simulated account; everything else is the real engine and API. This powers the public demo.

## What Interactive Brokers sends back

Recorded on the paper account on 2026-10-12 by `IbPaper.ComboFillsAreBookedAsLegs` (trades and ends flat; run in market hours):

```bash
set -a && . ./.env && set +a && cd engine/build && OPTRA_IB_PAPER_TESTS=1 OPTRA_IB_PAPER_FILLS=1 OPTRA_IB_PAPER_RAW=1 \
  ./tests/integration/test_ib_paper_gtest --gtest_filter='*ComboFillsAreBookedAsLegs*'
```

| Case | What IB does | What the engine does |
|---|---|---|
| a single option | several state reports, one execution, timestamped in IB Gateway's time zone | stamps each fill with the engine's time as it books it, as it stamps an order as it sends it |
| a combination (BAG) | three executions under one order number: the combination at its net price, then each leg; the order reported filled before the legs arrive | books only the legs |
| a combination of one option | no answer at all | sends a single option as an ordinary order |
| after reconnecting | resends the day's executions for the client number | drops those already booked; one client number per database |
| message 10197 | no market data: the live login the paper account shares data with is logged in elsewhere | |

```
secType=BAG  side=BOT price=2.3                           the combination: the net per unit
secType=OPT  SPXW 261013C07815000  side=BOT price=12.4    the first leg
secType=OPT  SPXW 261013C07820000  side=SLD price=10.1    the second leg
```

## Building and testing

```bash
./build.sh r                                   # Release: the programs and the numerics library
./build.sh r 0 g                               # a clean build with the Interactive Brokers connection
cmake --build engine/build -j4                 # the tests and benchmarks too (build.sh builds only the programs)
ctest --test-dir engine/build                  # the unit tests: no network, no database file
scripts/bench.sh --check                       # the latency benchmarks against their baseline
scripts/test_sanitize.sh thread                # the threaded tests under ThreadSanitizer (or address)
```

- Linux, GCC 14 or newer, the packages in the README; `build.sh` fetches the TWS API when needed.
- Unit tests: booking, holdings and P&L, risk, settlement, hedging, restarts, shutdown, reconnecting, the IB adapter against a fake broker, the strategy framework. Paper-account tests (`ctest -L ib_paper`) run by hand in market hours.
- On synthetic days, with the server's tests ([`test_backtest_truth.py`](../app/server/tests/trading/test_backtest_truth.py)): every fill is the next bar's quote and every day's P&L exact to the cent; changing every quote after a minute does not change what a strategy did before it.
- Every engine change is checked against the example strategies' last backtest results.
- The latency benchmarks fail when a hot path is more than 15% slower at p50, or 50% at p99, than its baseline at a fixed clock.
