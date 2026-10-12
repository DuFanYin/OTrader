# Research

The research side of Optra is where trade ideas are found and tested before they reach the paper account. You can price an option every way at once, calibrate a volatility model, stress a position, compare hedging schemes, and screen whole option chains for combinations that look cheap. Then you can turn a pick into a rule and backtest it on every recorded day.

A person uses it through the Research tabs of the page. An AI agent uses the same functions, called tools, over the server's API. Nothing here places an order.

![Risk: the stress library, the delta-hedged P&L distribution and the hedge efficiency frontier](images/research/risk.png)

## The research views

**Backtest** runs any strategy over a range of recorded days and shows the summary, the cumulative P&L, the Greeks it carried and each day's result. It is the same engine that trades live ([engine.md](engine.md#backtests)).

**Screener** starts from a chain of options and finds combinations worth a look. It builds single calls, straddles, strangles, iron condors and calendars, filters them (by price, liquidity, spread, days to expiry, moneyness, cost, risk-reward, delta, volatility) and ranks them by *edge*: the model's value of the combination minus what it would cost to trade at the quoted prices. The chain can be the live BTC or ETH chain from Deribit, or SPXW and SPY at any minute of a recorded day. A pick can be sent to Multi-Leg or Risk with one click, or backtested as a rule over every day, with and without a hedge.

**Pricing** prices one option with every method side by side (Black-Scholes, Monte Carlo, binomial and trinomial trees) with all five Greeks, the differences between the methods, the implied volatility and a Heston calibration, a scenario sweep, Greek surfaces, and the market's volatility smile and term structure. It can also price a whole grid of spot and volatility shocks.

**Multi-Leg** prices any combination of legs, recognises what it is (a straddle, a butterfly, an iron condor, ...) and adds up the Greeks.

**Risk** puts an option or a whole position through stress scenarios (crashes, volatility spikes, rate moves, at three severities), explains each scenario's P&L by spot, volatility and rate, and compares four ways of delta-hedging it on the same simulated paths.

**Numerics** is for the methods themselves: finite-difference PDE pricing, how fast the trees converge, a benchmark of every method against a chosen reference, and the change of measure from real-world to risk-neutral.

**Validation** checks the numerical machinery against theory: sample moments, Itô's formula on simulated paths, simulation and hedging sanity, and the trees against closed forms. Each failed check comes with a suggested fix.

Every view opens with a sample result, so you can see what it does before running anything. Inputs that need more room (legs, filters, method settings) open in dialogs. Every run is kept, and the History drawer can bring any earlier run back or compare two.

| Screener (dark theme) | Pricing |
|---|---|
| ![The screener ranking BTC strangles by edge](images/research/screener.png) | ![Pricing: every method side by side, Greeks and calibration](images/research/pricing.png) |

## Can the numbers be trusted?

The research side is built so that you can check it.

- **Methods check each other.** A price is shown by several independent methods at once, with the differences between them. An American option is valued by trees, finite differences and Longstaff-Schwartz Monte Carlo, with the early-exercise premium.
- **A validation gate can run before any computation.** It tests the numerics against theory with the current settings and can block a run that would not be reliable, saying which setting to change (more paths, finer steps, ...).
- **Results come with warnings.** A stale or offline option chain, wide or crossed quotes, marks outside their own quotes, a calibration whose parameters ended at their bounds, a hedge that had no quotes for its instrument: each is reported next to the result it affects.
- **Results are reproducible.** Parallel computations give bit-for-bit the same answer whatever the number of threads, and every result has a run id that can be looked up, rerun and compared later.

## Market data

| Data | From | When it is unavailable |
|---|---|---|
| BTC and ETH spot | Binance | a fixed value |
| BTC implied-volatility index (DVOL) | Deribit | 80% |
| Full BTC and ETH option chains | Deribit | the latest saved snapshot, then a recorded one shipped with the code |
| Funding rate, as an annual drift | Binance futures | a typical value |
| Interest rates, 1 month to 2 years | the US Treasury's daily yield curve | a fixed curve |
| SPXW and SPY option chains | the engine's recorded and backtest days | — |

No API keys are needed. Each source fails independently: if one is down, its fallback is used, the others carry on, and the result says which source it used.

A few details matter for the numbers:

- **The Treasury curve** is slow to fetch (about 18 seconds) and changes once a day, so the server fetches it in the background and keeps it. A request only waits for it when there is no curve less than a day old.
- **Deribit chains** are converted to USD with each row's own underlying price, each expiry's forward is the median across its rows, and Deribit's own rate is used so Greeks line up with the exchange's marks. Chains are cached for a minute and saved, 20 per currency, for offline use.
- **SPXW and SPY chains** come from the engine's recorded days and are priced exactly the way the engine prices a live snapshot: mid prices, each expiry's forward from put-call parity, implied volatilities from the engine's own Black function. A recorded day is converted on demand, so today's is as fresh as the last minute recorded.

## The methods

For readers who want to know exactly what is computed.

**Pricing.** Black-Scholes in closed form (calls and puts, dividend yield, a currency mode, digital calls). Monte Carlo with up to 20 million paths and three samplers: plain, antithetic, and Sobol quasi-random numbers. Binomial and trinomial trees, European and American. Finite differences (explicit, implicit, Crank-Nicolson; American exercise by PSOR), refining the time step automatically when an explicit scheme would be unstable. Longstaff-Schwartz for American options. A batch of up to 500 options in one call.

**Calibration.** Implied volatility by Brent's method. Heston prices from the characteristic function (in a form that avoids branch-cut problems), and a Heston fit to market prices by bounded Nelder-Mead, graded by its error as a share of the average quote so the grade means the same for a 10-dollar option and a 6,000-dollar one. The engine's ComboStrategy uses the very same calibration live.

**Stress.** Four scenario packs (Core 4, Vol First, Rates First, Crash Kit), each at three severities. Every scenario reprices with three methods, optionally hedges, and splits the P&L into spot, volatility and rate; a summary gives a robustness score, the key driver and the worst case. A position is stressed as a whole, each leg with its own volatility and forward. The live engine's holdings can be stressed the same way.

**Hedging.** A position (by default one option sold, as a market maker would) is delta-hedged on simulated paths four ways: at fixed steps, with transaction costs, only when delta has moved past a threshold, and with deltas from the wrong volatility. Each gives the P&L distribution, its tail (VaR and expected shortfall at 95%), turnover and costs; the page plots cost against tail risk to show which schemes are efficient. For a combination on real days, the engine's own hedge is compared instead.

**Screener.** Strikes are judged against each expiry's forward, Greeks use Black-76, and costs use the price you would actually trade at (the ask to buy, the bid to sell) unless you ask for the mid. The model value can come from the option's mark, a flat volatility, an implied-volatility surface, or a Heston model fitted to the chain itself. About two million iron condors are enumerated in about 12 ms.

**Theory.** Normal-distribution moments from a seeded sample; Itô's formula checked on discrete paths, where the gap shrinks like the square root of the time step; Brownian and Vasicek paths; the Radon-Nikodym density and a real-world against risk-neutral comparison.

## For agents

Every computation is a **tool**: a name, a description written for a language model, inputs with every field explained (with units), and an example. There are 36, including six that reach the engine (the days available, the strategy classes, strategy reports, a combination backtested as a rule, its hedge study, and the live holdings under stress).

Tools are plain HTTP on the server, the same API the page uses. `GET /api/tools` lists them all with their descriptions and input schemas; `POST /api/tools/<name>` runs one:

```bash
curl -s http://127.0.0.1:8080/api/tools/price_option -H 'content-type: application/json' \
  -d '{"spot": 100, "strike": 105, "rate": 0.03, "vol": 0.2, "maturity": 0.5}'
```

From Python, inside the server's environment, the same tools are functions:

```python
from optra.tools import call_sync
r = call_sync("price_option", spot=100, strike=105, rate=0.03, vol=0.2, maturity=0.5)
r["result_summary"]["bs"]
```

Every answer has the same shape: the headline numbers (`result_summary`), the tables and curves (`result_details`), the `warnings`, how it was computed (`diagnostics`), the inputs as understood with defaults filled in, and a `run_id`. A mistake in the inputs is answered with what is wrong, field by field; an unknown field is an error too, so a misspelt name is never silently ignored.

Every result is kept (in `~/.optra/runs.db`), so an agent can cite a run id and anyone can look it up, rerun it to see what changed, or compare two runs.

Units are the same everywhere: rates and volatilities are decimals per year (0.05 is 5%), maturities are in years, and prices are in the units of the underlying.

The agent tasks in `app/server/tests/evals` have answers that can be checked. `tests/evals/run.py` gives them to Claude with the tools; when a task fails, the fix is a better tool description or error message, not a different task.

## How it is built

Three layers, and the split is strict:

- **The C++ numerical library** (`engine/quant`) does every computation and nothing else: no network, no files.
- **The server** (`app/server`) fetches market data, checks inputs, calls the library through its C interface, adds warnings, and keeps the history. It does no arithmetic of its own.
- **The page** (`app/web`) shows inputs and results. It computes nothing.

A new capability therefore travels up the stack: a numerical routine, a workflow around it, a result structure, a C entry point, a Python wrapper, a tool with its described inputs, and finally a view. The library and the server share a version number for the shape of their answers; if the server finds a library built for another version, it refuses to use it and says the engine needs rebuilding.

## Tests and verification

From `app/server`, `uv run --group dev --group databento pytest -q tests/tools` runs the research tests offline in about a minute: the pricing methods, hedging, the theory checks, the screener and its routes, the option chains and their fallbacks, every tool's description and example, the run history, and the agent evals. A live check against Deribit runs with `OPTRA_DERIBIT_LIVE=1`, and a comparison with the QF-205 course package runs when that package is available.

Results recorded when the methods were added:

- The trees agree with QF-205 to 2e-13 and finite differences to 1e-14. The one larger difference, in American PSOR, comes from a QF-205 bug (it counts a boundary term twice).
- The trinomial tree converges at order −1. Longstaff-Schwartz with 100,000 paths is within 1% of a 2,000-step tree; the American PDE within 0.5%.
- The ported screener matched the original program row for row, apart from the defects deliberately fixed.

## Where the code came from

The research side grew out of three earlier projects: an option screener in C++ (its filters, generators and strategies, with several defects fixed: wrong maximum gain and loss for iron condors, direction ignored, filters rewriting shared data), the QF-205 course's Python code (the trinomial tree and the finite-difference schemes), and an option-pricing project in C++ (Longstaff-Schwartz, and the queues and thread pool now used only in the concurrency benchmark). It started as a companion to a nine-session course on risk-neutral pricing, and every topic of that course, from Brownian motion and Itô calculus to Girsanov's theorem and dynamic hedging, still has a tool you can run.
