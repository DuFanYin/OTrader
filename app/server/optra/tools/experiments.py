"""Experiments as tools: submit one (a strategy, a space of its settings, days and costs, under a family), follow it,
read its trials and their days, see what its results say once the tries are counted, and queue the
robustness checks that need new runs. Experiments run in the background on workers (research.worker); submitting
returns at once. See optra.research: the ledger, the analysis.
"""

from __future__ import annotations

from datetime import date
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field

from optra.research import analysis, combo, ledger, optimizer
from optra.research import backtest as research_backtest
from optra.research.ledger import Costs
from optra.tools.base import COSTS, Result, Tool, ToolError


class Input(BaseModel):
    model_config = ConfigDict(extra="forbid")


class SubmitRequest(Input):
    family: str = Field(..., description="The hypothesis the trials are variants of (e.g. '0DTE straddle timing'). Its "
                                         "trials are counted together, against its budget (200 unless raised); "
                                         "every trial in the ledger, whatever its family, deflates the best.")
    strategy: str = Field(..., description="A strategy class (strategy_classes lists them with their parameters).")
    space: dict[str, Any] = Field(default_factory=dict, description=(
        "Per parameter: a number (fixed), a list of values, {'values': [...]}, or {'range': [lo, hi]} with an "
        "optional 'step' (a grid of it) or without (sampled). Parameters left out keep their defaults."))
    search: Literal["grid", "random", "latin"] = Field(
        "grid", description="grid: every combination (ranges need a step); random: n samples; latin: n samples "
                            "of a Latin hypercube (each range cut into n strata, each sampled once).")
    n: int = Field(0, ge=0, le=100000, description="With random or latin: how many settings.")
    seed: int = Field(0, description="With random or latin: the seed (the same seed, the same settings).")
    symbol: str = Field("SPXW", description="The underlying whose days to run.")
    start: date | None = Field(None, description="The first day; none: the first there is.")
    end: date | None = Field(None, description="The last day; none: the last before the holdout.")
    source: Literal["both", "backtest", "recorded"] = Field("backtest", description="Historical days, recorded "
                                                                                       "ones, or both.")
    note: str = Field("", description="What the experiment is for.")
    costs: Costs = COSTS


class OptimizeRequest(Input):
    family: str = SubmitRequest.model_fields["family"]
    strategy: str = SubmitRequest.model_fields["strategy"]
    space: dict[str, Any] = Field(..., description=(
        "The parameters to search, each {'range': [lo, hi]} within its bounds; the others keep their defaults or "
        "fixed's values."))
    fixed: dict[str, float] = Field(default_factory=dict, description="Other parameters held at these values.")
    objective: Literal["sharpe", "net", "expectancy_r"] = Field(
        "sharpe", description="What it maximises: a trial's daily Sharpe ratio, its net P&L, or its mean R.")
    rounds: int = Field(5, ge=1, le=50, description="Rounds, each proposed from every result before it.")
    batch: int = Field(16, ge=1, le=256, description="Settings a round tries (one batch of the engine).")
    seed: int = Field(0, description="The same seed, the same proposals on the same results.")
    symbol: str = SubmitRequest.model_fields["symbol"]
    start: date | None = SubmitRequest.model_fields["start"]
    end: date | None = SubmitRequest.model_fields["end"]
    note: str = SubmitRequest.model_fields["note"]
    costs: Costs = COSTS


class ExperimentRequest(Input):
    experiment_id: str = Field(..., description="The experiment's id (submit_experiment returns it).")


class ListRequest(Input):
    limit: int = Field(20, ge=1, le=500, description="The latest this many.")


class AnalysisRequest(ExperimentRequest):
    train: int | None = Field(None, ge=2, description="Walk-forward: days to pick the best trial on (default a "
                                                      "third of the days).")
    test: int | None = Field(None, ge=1, description="Walk-forward: days to trade it on after (default a sixth).")


class RobustnessRequest(ExperimentRequest):
    trial_id: str | None = Field(None, description="The trial to check; none: the best by Sharpe ratio.")


class TrialRequest(Input):
    trial_id: str = Field(..., description="A trial's id (get_experiment lists them).")


class ReplayRequest(TrialRequest):
    days: list[date] | None = Field(None, max_length=20, description="The days to show; none: its worst.")
    worst: int = Field(5, ge=1, le=20, description="Without days: how many of its worst days to show.")


class BudgetRequest(Input):
    family: str = Field(..., description="The family.")
    budget: int = Field(..., ge=0, le=1000000, description="How many trials it may register in all.")


def _bad(exc: Exception) -> ToolError:
    return ToolError(str(exc), 400)


def submit_experiment(p: SubmitRequest) -> Result:
    try:
        cut = ledger.holdout_from(p.symbol)
        end = p.end
        if end is None and cut is not None:
            end = date.fromordinal(cut.toordinal() - 1)
        files = combo.days(p.symbol, p.start, end, p.source)
        out = ledger.submit(
            family=p.family, strategy=p.strategy, days=[str(f) for f in files], space=p.space,
            search={"kind": p.search, "n": p.n, "seed": p.seed},
            costs=p.costs, note=p.note)
    except ValueError as exc:
        raise _bad(exc) from exc
    return Result(summary=out, notes=["The trials run in the background (research.worker): follow them with "
                                      "get_experiment, then read experiment_analysis."])


def optimize(p: OptimizeRequest) -> Result:
    try:
        cut = ledger.holdout_from(p.symbol)
        end = p.end if p.end is not None or cut is None else date.fromordinal(cut.toordinal() - 1)
        files = combo.days(p.symbol, p.start, end, "backtest")
        out = optimizer.optimize(family=p.family, strategy=p.strategy, days=[str(f) for f in files], space=p.space,
                                 costs=p.costs, objective=p.objective, rounds=p.rounds, batch=p.batch, seed=p.seed,
                                 note=p.note, fixed=p.fixed)
    except ValueError as exc:
        raise _bad(exc) from exc
    return Result(summary=out, notes=["Every round is an experiment of the family, its trials counted; "
                                      "experiment_id holds every setting tried: read experiment_analysis and run "
                                      "experiment_robustness on it, as on any experiment."])


def list_experiments(p: ListRequest) -> Result:
    return Result(summary={"experiments": ledger.experiments(p.limit), "families": ledger.families()})


def get_experiment(p: ExperimentRequest) -> Result:
    try:
        out = ledger.experiment(p.experiment_id)
    except KeyError as exc:
        raise ToolError(f"no experiment {p.experiment_id}", 404) from exc
    done = [t for t in out["trials"] if t["summary"]]
    top = sorted(done, key=lambda t: t["summary"]["sharpe"], reverse=True)[:10]
    return Result(summary={k: out[k] for k in ("id", "family", "strategy", "days", "first_day", "last_day", "jobs",
                                               "family_counts", "note", "parent")}
                  | {"trials": len(out["trials"]), "done": len(done), "best": top},
                  details={"trials": out["trials"], "spec": out["spec"]})


def cancel_experiment(p: ExperimentRequest) -> Result:
    return Result(summary=ledger.cancel(p.experiment_id))


def experiment_analysis(p: AnalysisRequest) -> Result:
    try:
        out = analysis.analysis(p.experiment_id, p.train, p.test)
    except KeyError as exc:
        raise ToolError(f"no experiment {p.experiment_id}", 404) from exc
    notes = ["deflated_sharpe.dsr: the probability that the best trial's Sharpe ratio beats the best of "
             "`tries` tries with no skill (every trial the ledger had registered, in any family but the "
             "control's); above 0.95 is the bar.",
             "pbo.pbo: how often the trial best in sample falls in the bottom half out of sample; below 0.2 is the "
             "bar, 0.5 is what noise gives.",
             "walk_forward: the best trial of each training window traded on the next one, strung together."]
    return Result(summary={k: out.get(k) for k in ("trials", "days", "family", "tries", "family_tries", "best")}
                  | {"dsr": (out.get("deflated_sharpe") or {}).get("dsr"), "pbo": (out.get("pbo") or {}).get("pbo"),
                     "walk_forward_net": (out.get("walk_forward") or {}).get("net")},
                  details=out, notes=notes)


def experiment_robustness(p: RobustnessRequest) -> Result:
    try:
        out = analysis.robustness(p.experiment_id, p.trial_id)
    except KeyError as exc:
        raise ToolError(f"no such experiment or trial: {exc}", 404) from exc
    except ValueError as exc:
        raise _bad(exc) from exc
    return Result(summary=out, notes=["Each check is an experiment of its own (parent = this one); compare their "
                                      "trials with get_experiment once they are done."])


def get_trial(p: TrialRequest) -> Result:
    days = ledger.trial_days([p.trial_id])[p.trial_id]
    if not days:
        raise ToolError(f"no finished trial {p.trial_id}", 404)
    written: dict[str, list[float]] = {}
    for d in days:
        for name, value in (d.get("values") or {}).items():
            written.setdefault(name, []).append(value)
    return Result(summary={"trial_id": p.trial_id, "days": len(days),
                           "net": round(sum(d["net_pnl"] for d in days), 2)}
                  | ({"values": {name: {"days": len(v), "mean": sum(v) / len(v)} for name, v in written.items()}}
                     if written else {}),
                  details={"days": days, "trades": ledger.trial_trades([p.trial_id])[p.trial_id]})


def replay_trial(p: ReplayRequest) -> Result:
    try:
        out = research_backtest.replay(p.trial_id, p.days, p.worst)
    except KeyError as exc:
        raise ToolError(f"no trial {p.trial_id}", 404) from exc
    except ValueError as exc:
        raise _bad(exc) from exc
    notes = ["Each day: its fills (time, contract, side, price), its P&L by source, the round trips it opened, and "
             "the log: what the strategy wrote (write_log), its orders and fills, the engine's refusals."]
    if out["differs_from_ledger"]:
        notes.append(f"The replay's P&L differs from the ledger's on {len(out['differs_from_ledger'])} days: the "
                     "trial's data or its strategy is not deterministic.")
    return Result(summary={k: out[k] for k in ("trial_id", "strategy", "setting", "days_run", "errors",
                                               "differs_from_ledger")}
                  | {"days": [{k: d[k] for k in ("day", "net", "attribution")} for d in out["days"]]},
                  details={"days": out["days"], "costs": out["costs"]}, notes=notes)


def set_family_budget(p: BudgetRequest) -> Result:
    try:
        return Result(summary={p.family: ledger.set_budget(p.family, p.budget)})
    except ValueError as exc:
        raise _bad(exc) from exc


EXPERIMENT = ("Experiments: many backtests of one idea, each registered (and counted against its family) before it "
              "runs, run in the background. ")
EXAMPLE_ID = "3f2c9a5e0b1d"  # the examples' stand-ins for a real experiment and trial
EXAMPLE_TRIAL = "0123456789abcdef"

TOOLS = [
    Tool("submit_experiment", "Submit an experiment",
         EXPERIMENT + "For a sweep too long to wait for (backtest is the usual way, and waits): submits a strategy "
         "over a space of its settings (a grid, random or Latin hypercube samples) on "
         "an underlying's days before its holdout, with costs, under a family. Returns at once with the experiment's "
         "id and how many trials it queued and reused (a trial already run is not run again). Refused when the "
         "family's budget would be passed or a day is in the holdout.",
         SubmitRequest, submit_experiment, "/experiments", cost="seconds", engine=False,
         example={"family": "random buys", "strategy": "RandomBuyStrategy", "symbol": "SPY",
                  "space": {"seed": [1, 2], "hold_minutes": {"range": [30, 60], "step": 30}}}),
    Tool("optimize", "Optimise a strategy's settings",
         EXPERIMENT + "Bayesian optimisation over ranges of a strategy's parameters on an underlying's days before "
         "its holdout: each round's settings proposed from every result before it (a Gaussian process, expected "
         "improvement), run as an experiment of the family and counted against its budget; waits for all its "
         "rounds. Returns the experiment of every setting tried (what the analysis and the promotion gates read) "
         "and the best setting by the objective.",
         OptimizeRequest, optimize, "/experiments/optimize", cost="heavy", engine=False,
         example={"family": "random buys", "strategy": "RandomBuyStrategy", "symbol": "SPY", "rounds": 2,
                  "batch": 4, "space": {"hold_minutes": {"range": [15, 90]}}}),
    Tool("list_experiments", "List experiments",
         EXPERIMENT + "The latest experiments (trials done, failed, pending) and every family with its budget and "
         "the trials it has used.",
         ListRequest, list_experiments, "/experiments", method="GET", engine=False, record=False,
         example={"limit": 10}),
    Tool("get_experiment", "Look up an experiment",
         EXPERIMENT + "An experiment: its spec, its tasks by status, its family's counts, its ten best trials by "
         "Sharpe ratio; result_details.trials has every trial with its setting and headline numbers.",
         ExperimentRequest, get_experiment, "/experiments/{experiment_id}", method="GET", engine=False,
         record=False, example={"experiment_id": EXAMPLE_ID}),
    Tool("cancel_experiment", "Cancel an experiment",
         EXPERIMENT + "Drops the experiment's tasks not yet started; their trials stay counted.",
         ExperimentRequest, cancel_experiment, "/experiments/{experiment_id}/cancel", engine=False, record=False,
         example={"experiment_id": EXAMPLE_ID}),
    Tool("experiment_analysis", "Analyse an experiment",
         EXPERIMENT + "What the finished trials say once the tries are counted (every trial in the ledger, whatever "
         "its family): the best trial's deflated "
         "Sharpe ratio, the probability of backtest overfitting (combinatorially symmetric cross-validation), a "
         "walk-forward of picking the best on each window, and the best trial without its five best days and by "
         "the underlying's move (up, down, quiet, wild days).",
         AnalysisRequest, experiment_analysis, "/experiments/{experiment_id}/analysis", method="GET",
         cost="seconds", engine=False, example={"experiment_id": EXAMPLE_ID}),
    Tool("experiment_robustness", "Robustness checks",
         EXPERIMENT + "Queues the checks that need new runs on a trial (the best unless named): its costs doubled, "
         "its fills held to the size shown and a minute late, each parameter the experiment varied moved 20% "
         "down and up, and the same setting on the other underlying over the same dates (SPXW for SPY, SPY for SPXW "
         "and XSP; reported, not a gate). Each is an experiment under the same "
         "family, so it counts against the budget.",
         RobustnessRequest, experiment_robustness, "/experiments/{experiment_id}/robustness", cost="seconds",
         engine=False, example={"experiment_id": EXAMPLE_ID}),
    Tool("get_trial", "Look up a trial",
         EXPERIMENT + "A finished trial's days (P&L, fees, P&L by source, flat at the close) and its round trips "
         "(the day each opened, days open, net, risk, R); the values its strategy wrote each day (write_value: what a "
         "strategy that only measures answers with), each day's under its \"values\".",
         TrialRequest, get_trial, "/experiments/trials/{trial_id}", method="GET", engine=False, record=False,
         example={"trial_id": EXAMPLE_TRIAL}),
    Tool("replay_trial", "Replay a trial",
         EXPERIMENT + "Why a trial made or lost what it did: runs it again as it ran (the same strategy, setting, "
         "costs, days and code; not counted again, and checked against the ledger) and shows, on the days asked for "
         "or its worst, each fill with its time, contract, side and price, the day's P&L by source, the round trips "
         "it opened, and what its strategy logged (write_log), with its orders and the engine's refusals. Days in the "
         "holdout are not shown.",
         ReplayRequest, replay_trial, "/experiments/trials/{trial_id}/replay", cost="heavy", engine=False,
         record=False, example={"trial_id": EXAMPLE_TRIAL, "worst": 3}),
    Tool("set_family_budget", "Set a family's budget",
         EXPERIMENT + "Sets how many trials a family may register in all. Raising it is a decision to look further "
         "at a hypothesis that has used its tries: the deflated Sharpe ratio counts them all.",
         BudgetRequest, set_family_budget, "/experiment_families", engine=False, record=False,
         person=True,
         example={"family": "0DTE straddle timing", "budget": 400}),
]
