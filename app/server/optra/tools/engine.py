"""The trading engine's tools: the backtest days and strategy classes there are, the cache of priced days, a report of
strategies over backtest days, a combo from the screener backtested as ComboStrategy, the hedge study, and the
live holdings under stress. Declared like every tool, so reached the same ways (HTTP and Python);
results come in the one envelope and, but for the lookups, are kept in the run
history (list_runs, get_run, compare_runs).

None of them places an order: a backtest's result carries the strategy as POST /api/strategies takes
it, and the operator adds it to the paper account.
"""

from __future__ import annotations

from datetime import date
from pathlib import Path
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field

from optra import settings
from optra.research.ledger import Costs
from optra.tools.base import COSTS, Result, Tool, ToolError

from optra.research import backtest as research_backtest
from optra.research import batch, combo, configs, jobs, report
from optra.services import stress
from optra.data import chains

HOLDOUT = Field(None, description="Days from this one on are the holdout, reported apart from the research days.")


class Input(BaseModel):
    model_config = ConfigDict(extra="forbid")


class DaysRequest(Input):
    symbol: str | None = Field(None, description="An underlying (SPXW, SPY); none: every one with days.")


class NoInput(Input):
    pass


class DayRange(Input):
    symbol: str = Field("SPXW", description="The underlying: SPXW or SPY (backtest_days lists what there is).")
    start: date | None = Field(None, description="The first day; none: the first there is.")
    end: date | None = Field(None, description="The last day; none: the last there is.")
    source: Literal["both", "backtest", "recorded"] = Field(
        "both", description="Days from historical data (backtest), days the live engine recorded (recorded), or both "
                            "(a recorded day wins).")


class BacktestRequest(Input):
    config: str | None = Field(None, description="Run strategy/configs/<config>.json (put_config writes one; "
                                                 "research_configs lists them) instead of the fields below: give it "
                                                 "alone.")
    strategy: str | None = Field(None, description="A strategy class (strategy_classes lists them with their "
                                                   "parameters; put_strategy writes a new one).")
    setting: dict[str, float] | None = Field(None, description="One setting: parameter -> value; parameters left out "
                                                               "keep their defaults. Give this or space.")
    space: dict[str, Any] | None = Field(None, description=(
        "A space of settings instead: per parameter a number (fixed), a list of values, or {'range': [lo, hi]} with an "
        "optional 'step'. Every setting tried is a trial counted in the ledger."))
    search: Literal["grid", "random", "latin"] = Field("grid", description="With space: grid (every combination), "
                                                                           "random or latin (n samples).")
    n: int = Field(0, ge=0, le=10000, description="With random or latin: how many settings.")
    seed: int = Field(0, description="With random or latin: the seed.")
    family: str | None = Field(None, description="The hypothesis the settings are variants of (default: the class); "
                                                 "its trials are held to its budget. What deflates the best is every "
                                                 "trial in the ledger, whatever its family.")
    symbol: str = Field("SPXW", description="The underlying whose days to run.")
    start: date | None = Field(None, description="The first day; none: the first there is.")
    end: date | None = Field(None, description="The last day; none: the last before the holdout.")
    note: str = Field("", description="What the backtest is for.")
    costs: Costs = COSTS


class ReportRequest(Input):
    days: str | None = Field(None, description="A folder of backtest days, or one day, relative to the repository; "
                                               "or leave it out and give symbol (with start, end, source).")
    symbol: str | None = Field(None, description="The underlying whose days to run (instead of days).")
    start: date | None = Field(None, description="With symbol: the first day.")
    end: date | None = Field(None, description="With symbol: the last day.")
    source: Literal["both", "backtest", "recorded"] = Field(
        "both", description="With symbol: days from historical data (backtest), recorded by the live engine "
                            "(recorded), or both (a recorded day wins).")
    strategies: list[str] = Field(description='Each a class (strategy_classes lists them), or '
                                              '"Class:name=value,name=value" with its parameters.')
    holdout_from: date | None = HOLDOUT
    costs: Costs = COSTS


class ComboLeg(Input):
    option_type: Literal["call", "put"] = Field(description="The leg's right.")
    ratio: int = Field(description="Contracts per combo unit: bought +, sold - (the screener's qty).", ge=-10, le=10)
    strike_pct: float | None = Field(None, ge=-50, le=50, description=(
        "The strike as a percent from its expiry's forward (the nearest listed is taken). Give it with dte, or give "
        "strike, expiry and forward (a contract as the screener or Multi-Leg shows it) with as_of."))
    dte: int | None = Field(None, ge=0, le=365, description="Its expiry: the first at least this many days out.")
    strike: float | None = Field(None, gt=0, description="The contract's strike (with expiry, forward and as_of).")
    expiry: date | None = Field(None, description="The contract's expiry.")
    forward: float | None = Field(None, gt=0, description="The forward its expiry was priced on, on as_of.")


class ComboRequest(DayRange):
    costs: Costs = COSTS
    legs: list[ComboLeg] = Field(min_length=1, max_length=4, description="One to four legs.")
    as_of: date | None = Field(None, description="The day legs given by strike and expiry were picked on.")
    entry_minute: int = Field(30, ge=1, le=389, description="Minutes after the 09:30 New York open it opens (30: 10:00).")
    exit_minute: int = Field(380, ge=2, le=390, description="Minutes after the open it closes (380: 15:50).")
    quantity: int = Field(1, ge=1, le=100, description="Combo units.")
    take_profit: float = Field(0, ge=0, le=100, description="Close once the P&L reaches this times the premium; 0: never.")
    stop_loss: float = Field(0, ge=0, le=100, description="Close once it loses this times the premium; 0: never.")
    model: Literal["none", "smile"] = Field("none", description=(
        "smile: open only when the legs' value at the IVs on their chains' smiles (an SVI fitted to each chain's quotes "
        "every minute) less the cost at the executable prices is at least min_edge."))
    min_edge: float = Field(0, description="USD a combo unit, with a model.")
    holdout_from: date | None = HOLDOUT

    def setting(self, **hedge: float) -> dict[str, float]:
        return combo.setting([leg.model_dump(mode="json") for leg in self.legs], self.as_of,
                             entry_minute=self.entry_minute, exit_minute=self.exit_minute, quantity=self.quantity,
                             take_profit=self.take_profit, stop_loss=self.stop_loss,
                             model=1 if self.model == "smile" else 0, min_edge=self.min_edge, **hedge)

    def files(self) -> list[Path]:
        return combo.days(self.symbol, self.start, self.end, self.source)


class BacktestComboRequest(ComboRequest):
    hedge: bool = Field(False, description="The engine's delta hedge keeps its delta within delta_range shares of zero.")
    delta_range: int = Field(20, ge=0, le=1000, description="Shares either side of zero left alone.")
    hedge_timer: int = Field(1, ge=1, le=390, description="Hedge every this many minutes.")


class HedgeStudyRequest(ComboRequest):
    delta_ranges: list[int] = Field([5, 20, 50], min_length=1, max_length=8,
                                    description="The bands to compare, shares either side of zero; unhedged is always run.")
    hedge_timer: int = Field(1, ge=1, le=390, description="Hedge every this many minutes.")


class StressRequest(Input):
    pack: str = Field("core4", description="The stress pack (stress_packs lists them).")
    severity: Literal["mild", "moderate", "severe"] = Field(
        "moderate", description="How hard the pack's shocks hit: mild 0.7x, moderate 1x, severe 1.5x.")
    account: Literal["paper", "live"] = Field("paper", description="Whose engine's holdings: the paper account's or "
                                                                     "the live one's.")


def _bad(exc: Exception) -> ToolError:
    return ToolError(str(exc), 400)


def backtest_days(p: DaysRequest) -> Result:
    names = [p.symbol] if p.symbol else chains.symbols()
    out = {}
    for name in names:
        found = chains.day_files(name)
        out[name] = {"days": len(found), "first": min(found, default=None), "last": max(found, default=None),
                     "recorded": sorted(d for d, (_, kind) in found.items() if kind == "recorded")}
    return Result(summary={"symbols": out})


def _config(name: str) -> BacktestRequest:
    try:
        return BacktestRequest.model_validate(configs.read(name))
    except FileNotFoundError as exc:
        raise ToolError(str(exc), 404) from exc
    except ValueError as exc:  # a bad name, a file that is not a request
        raise ToolError(f"strategy/configs/{name}.json: {exc}", 400) from exc


def _checked(p: BacktestRequest) -> BacktestRequest:
    """The request to run: the config named, or the one given."""
    if p.config is not None:
        if p.model_fields_set - {"config"}:
            raise ToolError("give config alone: what runs is the file", 400)
        p = _config(p.config).model_copy(update={"config": p.config})
    if not p.strategy:
        raise ToolError("give a strategy (or a config)", 400)
    if p.setting is not None and p.space:
        raise ToolError("give setting or space, not both", 400)
    return p


def backtest(p: BacktestRequest) -> Result:
    p = _checked(p)
    search = {"kind": p.search, "n": p.n, "seed": p.seed}
    try:
        out = research_backtest.backtest(strategy=p.strategy, setting=p.setting, space=p.space, search=search,
                                         family=p.family, symbol=p.symbol, start=p.start, end=p.end, costs=p.costs,
                                         note=p.note, config=p.config)
    except ValueError as exc:
        raise _bad(exc) from exc
    summary = {k: out[k] for k in ("experiment_id", "config", "family", "tries", "family_tries", "days", "first_day", "last_day",
                                   "best",
                                   "control", "beats_control_by_r", "deflated_sharpe", "overfitting_probability",
                                   "walk_forward_net", "review")}
    stopped = out["stopped"]
    warnings = [f"{len(stopped)} of {len(out['trials'])} trials stopped before their last day, so their measures and "
                f"the selection statistics cover only the days before ({next(iter(stopped.values()))}); fill the data "
                "and run again"] if stopped else []
    return Result(summary=summary | {"trials": len(out["trials"]), "stopped": len(stopped)}, warnings=warnings,
                  details={k: out[k] for k in ("trials", "without_best_days", "by_regime", "robustness",
                                               "best_trades")},
                  notes=["tries: every trial the ledger had registered, in any family but the control's; the more "
                         "tried, the lower the deflated Sharpe of the best, whatever the family is called. review "
                         "lists what promote_strategy_version asks for to move a version to review; "
                         "register_strategy_version makes the best a version.",
                         "result_details.best_trades: the best's round trips; replay_trial shows a trial's fills, "
                         "entries and exits and its strategy's log on the days asked for."])


class PutConfigRequest(Input):
    name: str = Field(..., description="The config's name: strategy/configs/<name>.json (lower-case letters, digits, "
                                       "_).")
    request: BacktestRequest = Field(..., description="The backtest, as backtest takes it, without config.")


def put_config(p: PutConfigRequest) -> Result:
    if settings.demo():
        raise ToolError("not in the demo: it keeps no config a visitor writes", 403)
    if p.request.config is not None:
        raise ToolError("a config names no other config", 400)
    request = _checked(p.request)
    body = request.model_dump(mode="json", exclude={"config"})
    try:
        settings_ = configs.settings_of(body)
        path = configs.write(p.name, body)
    except ValueError as exc:
        raise ToolError(str(exc), 400) from exc
    family = request.family or request.strategy
    left = configs.family_left(family)
    notes = [f"backtest with config {p.name!r} runs it; until then nothing is registered or counted."]
    if left is not None and len(settings_) > left:
        notes.append(f"{family} may register {left} more trials, fewer than these {len(settings_)} (the trials the "
                     "ledger already has are not counted again).")
    return Result(summary={"config": p.name, "file": str(path.relative_to(configs.config_dir().parent.parent)),
                           "strategy": request.strategy, "family": family, "settings": len(settings_),
                           "family_trials_left": left},
                  details={"settings": settings_, "request": body}, notes=notes)


def research_configs(_: NoInput) -> Result:
    rows = []
    for name in configs.names():
        body, settings_, problem = {}, None, None
        try:
            body = configs.read(name)
            body = BacktestRequest.model_validate(body).model_dump(mode="json", exclude={"config"})
            settings_ = len(configs.settings_of(body))
        except (ValueError, KeyError) as exc:  # not JSON, not a request, a class or parameter gone
            problem = str(exc)
        rows.append({"config": name, "strategy": body.get("strategy"), "family": body.get("family") or body.get("strategy"),
                     "symbol": body.get("symbol"), "settings": settings_, "note": body.get("note", ""),
                     "problem": problem, "runs": configs.runs(name)})
    return Result(summary={"configs": len(rows)}, details={"configs": rows})


def cache_backtest_days(p: DayRange) -> Result:
    if settings.demo():
        raise ToolError("not in the demo: its days are priced as its backtests read them", 403)
    if p.source == "recorded":
        raise ToolError("recorded days change while the engine records: they are not cached ahead", 400)
    try:
        files = combo.days(p.symbol, p.start, p.end, p.source)
    except ValueError as exc:
        raise _bad(exc) from exc
    batch = {"files": [str(f) for f in files], "risk_free_rate": 0.05, "iv_price_mode": "mid", "trials": []}
    summary = {"days": len(files), "first": files[0].name, "last": files[-1].name}
    if jobs.is_here():  # no workers (a script, the tests): cached now
        jobs.run(batch, jobs.PRIORITY_EXPERIMENT)
        return Result(summary=summary | {"job_id": None})
    job_id = jobs.submit(batch, jobs.PRIORITY_EXPERIMENT, name=f"cache:{p.symbol}")
    return Result(summary=summary | {"job_id": job_id},
                  notes=["A job of the research workers, behind every other: GET /api/jobs shows the queue."])


def backtest_report(p: ReportRequest) -> Result:
    try:
        if p.days:
            days: Path | list[Path] = Path(p.days)
        elif p.symbol:
            days = combo.days(p.symbol, p.start, p.end, p.source)
        else:
            raise ValueError("give days (a folder) or symbol")
        runs = {s: batch.parse_spec(s) for s in p.strategies}  # as written: each its row's name
        out = report.report(days, runs, p.holdout_from, p.costs)
    except ValueError as exc:
        raise _bad(exc) from exc
    return Result(summary={"days": out["days"], "strategies": len(p.strategies), "header": out["header"]},
                  details={"periods": out["periods"], "trades": out["trades"], "markdown": out["markdown"],
                           "errors": out["errors"]},
                  warnings=[f"{s}: {e}" for s, errs in out["errors"].items() for e in errs])


def _report_summary(out: dict[str, Any]) -> dict[str, Any]:
    rows = out["periods"][-1]["rows"]  # all days
    row = rows[0] if rows else {}
    return {"days": out["days"], **{k: row.get(k) for k in (
        "trades", "win rate", "expectancy R", "t", "net USD", "drawdown USD", "fees", "attribution")}}


def backtest_combo(p: BacktestComboRequest) -> Result:
    try:
        values = p.setting(hedge=int(p.hedge), delta_range=p.delta_range, hedge_timer=p.hedge_timer)
        out = combo.backtest(p.symbol, values, p.files(), p.holdout_from, p.costs)
    except ValueError as exc:
        raise _bad(exc) from exc
    return Result(summary=_report_summary(out),
                  details={"strategy": out["strategy"], "periods": out["periods"],
                           "trades": next(iter(out["trades"].values()), []), "header": out["header"]},
                  notes=["attribution: the P&L before fees by source (the engine's: delta, gamma, theta, vega, other, "
                         "fills against the mid); fees apart.",
                         "details.strategy is what POST /api/strategies takes to run the same on the paper account."],
                  warnings=[e for errs in out["errors"].values() for e in errs])


def hedge_study(p: HedgeStudyRequest) -> Result:
    try:
        out = combo.hedge_study(p.setting(), p.files(), p.delta_ranges, p.hedge_timer, p.costs)
    except ValueError as exc:
        raise _bad(exc) from exc
    return Result(summary={"least_tail_loss": out["least_tail_loss"],
                           "variants": [{k: r.get(k) for k in ("variant", "days", "mean", "std", "es95", "hedge_fills",
                                                               "fees")} for r in out["rows"]]},
                  details={"rows": out["rows"]},
                  notes=["Per variant, the daily P&L after fees: the hedging tool's measures (mean, std, q05/q50/q95, "
                         "var95, es95 = the mean loss of the worst 5% of days), on days that happened."],
                  warnings=[f"{v}: {e}" for v, errs in out["errors"].items() for e in errs])


def holdings_stress(p: StressRequest) -> Result:
    from optra.infra import live  # the account's engine's client, at call time

    try:
        holdings = live.client(p.account).get_strategy_holdings()["holdings"]
    except Exception as exc:  # no engine: say so as the tool's error
        raise ToolError(f"the {p.account} account's engine is not reachable: {getattr(exc, 'detail', exc)}",
                        503) from exc
    try:
        out = stress.stress(holdings, p.pack, p.severity)
    except ValueError as exc:
        raise _bad(exc) from exc
    worst = out["worst"] or {}
    return Result(summary={"worst_scenario": worst.get("scenario"), "worst_pnl": worst.get("pnl"),
                           "legs": out["legs"], "marked_at": out["marked_at"]},
                  details={k: out[k] for k in ("scenarios", "strategies", "total", "unpriced", "marked_on")},
                  warnings=out["warnings"])


ENGINE = "The trading engine's. "
COMBO_EXAMPLE = {"symbol": "SPXW", "legs": [{"option_type": "call", "ratio": 1, "strike_pct": 0, "dte": 0},
                                            {"option_type": "put", "ratio": 1, "strike_pct": 0, "dte": 0}]}

TOOLS = [
    Tool("backtest", "Backtest a strategy",
         ENGINE + "The way to test a strategy: one setting, or a space of settings, over the underlying's days before "
         "its holdout, with costs. Every setting is a trial registered and counted before it runs; the "
         "call waits for them and answers whole: each trial's measures (trades, win rate, expectancy in R with its t, "
         "net USD, drawdown), the control's (RandomBuyStrategy) on the same days and the best's gap to it, the deflated "
         "Sharpe and the overfitting probability given every trial in the ledger, a walk-forward, the best without its "
         "five best days and by the underlying's move; and when the best beats the control, its robustness checks "
         "(costs doubled, fills of the size shown a minute late, each parameter moved 20%, the other underlying: "
         "other_underlying, not a gate), run then too. review "
         "lists, met or not, what a version needs to move to review.",
         BacktestRequest, backtest, "/backtest", cost="heavy", engine=False,
         example={"strategy": "RandomBuyStrategy", "space": {"seed": [1, 2]}, "symbol": "SPXW",
                  "start": "2025-03-03", "end": "2025-03-07"}),
    Tool("put_config", "Write an experiment config",
         ENGINE + "Writes strategy/configs/<name>.json: one backtest as backtest takes it (strategy, setting or space, "
         "family, underlying and days, costs, note), every default written out, so that what runs later is the file. "
         "Checked, not run: refused when the class, a parameter or a value out of its range is wrong; else the "
         "settings it would try and the trials its family has left. backtest with config runs it.",
         PutConfigRequest, put_config, "/backtest/configs", cost="seconds", engine=False, record=False,
         example={"name": "example_straddle", "request": {
             "strategy": "ComboStrategy", "space": {"leg1_ratio": 1, "leg2_ratio": 1, "leg2_right": 1,
                                                    "entry_minute": [30, 150]},
             "family": "example straddles", "symbol": "SPXW", "start": "2025-03-03", "end": "2025-03-07",
             "note": "the example"}}),
    Tool("research_configs", "Experiment configs",
         ENGINE + "The configs in strategy/configs: each one's strategy, family, underlying, how many settings it "
                  "tries, its note, and the experiments that ran it (when, how many trials, the best net); a config "
                  "that no longer checks (a parameter gone) says why.",
         NoInput, research_configs, "/backtest/configs", method="GET", engine=False, record=False),
    Tool("backtest_days", "Backtest days",
         ENGINE + "The days there are to backtest on, per underlying: how many, the first and last, and which the live "
         "engine recorded.", DaysRequest, backtest_days, "/backtest/days", method="GET", engine=False,
         record=False, example={"symbol": "SPXW"}),
    Tool("cache_backtest_days", "Cache backtest days",
         ENGINE + "Queues a job that reads the days' contracts and minutes into the cache of backtest days; each "
         "chain of a day is priced and stored there the first time a backtest's strategy asks for it (the cache is "
         "per version of the pricing code). Returns at once with the job's id.",
         DayRange, cache_backtest_days, "/backtest/cache", cost="seconds", engine=False, record=False,
         example={"symbol": "SPXW", "start": "2025-03-03", "end": "2025-03-07", "source": "backtest"}),
    Tool("backtest_report", "Backtest report",
         ENGINE + "Strategies over "
         "backtest days, a trade a day: trades, win rate, average win and loss in R (R = the "
         "day's P&L after fees / what the position opened could lose at most), payoff, expectancy with its t, net USD, "
         "losing streak, drawdown, the gap to RandomBuyStrategy, the P&L by source (delta, gamma, theta, vega, other, "
         "fills); the research days and the holdout apart. The agent's report stops before the holdout: a version's holdout_report "
         "is its one way there. Returns result_details.periods (a row per strategy), "
         "trades (each day) and markdown.",
         ReportRequest, backtest_report, "/backtest/report", cost="heavy", engine=False,
         example={"symbol": "SPXW", "strategies": ["RandomBuyStrategy:seed=1", "RandomBuyStrategy:seed=2"]}),
    Tool("backtest_combo", "Backtest a combo",
         ENGINE + "A combo of one to four legs (from the screener or Multi-Leg, or rules: call or put, ratio, strike as "
         "a percent from the forward, days to expiry) opened each day at entry_minute and closed at exit_minute or "
         "on take_profit / stop_loss, run as the engine's ComboStrategy over the days; optionally hedged, and "
         "optionally only on a Heston edge. Returns the report's measures and P&L by source, each day, and "
         "result_details.strategy: the strategy as POST /api/strategies takes it, to run the same on paper.",
         BacktestComboRequest, backtest_combo, "/backtest/combo", cost="heavy", engine=False,
         example=COMBO_EXAMPLE),
    Tool("hedge_study", "Hedge study on backtest days",
         ENGINE + "The same combo over the same days unhedged and with its delta kept within each band: per variant "
         "the daily P&L after fees (mean, std, quantiles, var95, es95), the share fills, the fees and the P&L by "
         "source. The hedging tool's comparison, on days that happened instead of simulated paths. Its variants are "
         "trials in the ledger (the ComboStrategy family), counted like any; days before the holdout only.",
         HedgeStudyRequest, hedge_study, "/backtest/hedge-study", cost="heavy", engine=False,
         example={**COMBO_EXAMPLE, "delta_ranges": [5, 20, 50]}),
    Tool("holdings_stress", "Live holdings under stress",
         ENGINE + "Every live strategy's holding revalued under a stress pack and a spot ladder (+-1, 2, 5%), each "
         "option at its own implied vol and its expiry's forward from the newest quotes: P&L by strategy and in "
         "total, and the legs it could not value.",
         StressRequest, holdings_stress, "/risk/stress", method="GET", engine=True,
         example={"pack": "core4", "severity": "moderate"}, live=True),
]

