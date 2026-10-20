"""A strategy version's way from research to live, each step on evidence.

A version is a strategy class with its setting frozen and the code it is (versions.register_version). Everything that
happened to it is already in the ledger: its trials in experiments (by class, setting and code), the promotion
report's trials that reached the holdout, its paper days. This module reads that evidence, checks it against the
gates of the next stage, and moves the version only when they pass; the stages that put money at risk also need a
person's approval, given in words, and only the person may give it (infra/access.py): the agent asks for review,
the person approves paper, pilot and live. Going back (or retiring) needs only a reason, and anyone may. A move is
written before it is carried out (versions.set_pending), then done step by step, each step safe to repeat
(versions.advance): one that fails leaves it under way, and the session check carries it out again
(versions.advance_pending).

    research -> review -> paper -> pilot -> live, and retired from any of them

Where it trades, its code is installed (strategies.install) for the account's engine: for the paper account from
paper on, for the live one from pilot on; going back, or retiring, takes it out of what it no longer trades. An
agent's strategy reaches an account only this way, or through a person installing it (install_strategy). A demo
strategy is loaded by every engine already: nothing to install. On the paper engine (the server's) the version then
runs as a strategy of its own, named after it (versions.engine_name), with its setting frozen, on the underlying it
was tested on; going back stops it and takes it off. The daily report compares each of its paper days with a backtest of the
day, which the gate to pilot reads. Each daily report also holds every trading version's paper days against what its
research days predict (standing): one whose last weeks fall below the prediction goes back a stage on its own (watch).

The gates' numbers are the plan's starting values, to be tuned once there is data to tune them on.
"""

from __future__ import annotations

import random
import statistics
from dataclasses import dataclass
from datetime import date, datetime, timedelta, timezone
from typing import Any

from optra.data import daily_values
from optra.data.days import day_of
from optra.infra import access
from optra.research import analysis, batch, combo, evidence, ledger, report, versions

STAGES = ("research", "review", "paper", "pilot", "live", "retired")
NEEDS_APPROVAL = {"paper", "pilot", "live"}  # the stages where it trades: a person decides

MIN_TRADES = 300           # independent trades before anything is concluded
MIN_TRADE_DAYS = 300       # the days they opened on: trades of one day are not independent of each other
DSR_BAR = 0.95             # the deflated Sharpe ratio of the experiment that found it
PBO_BAR = 0.2              # the probability of backtest overfitting there
RAN_ALL_DAYS = "every trial ran all its days"  # one that stopped early (a missing day) measured only part
MIN_PAPER_DAYS = 20        # four weeks of paper trading
MAX_UNMATCHED = 0.1        # paper fills with no backtest fill to match, as a share of all
PILOT_DAYS = 56            # calendar days in the pilot before going live
MAX_SLIPPAGE = 1.5         # what paper fills paid over the mids, at most this times what the backtest's did
PREDICTED = (0.05, 0.95)   # the band of the research's P&L over as many days that paper must fall in
PREDICTION_DAYS = 20       # the last paper days watched against the band (watch)
MIN_PREDICTED_DAYS = 5     # paper days before a prediction says anything
HOLDOUT_REPORTS = 3        # holdout reports a family may have: past them the holdout would be one more validation set
KEPT_BY_DAY = 15           # values in the 30 days before the holdout: a series kept by day (an event list is not)
STALE_DAYS = 7             # calendar days a kept series' last value may be before the holdout's last day


@dataclass
class Gate:
    name: str
    passed: bool
    detail: str


def experiment_gates(experiment_id: str, a: dict[str, Any]) -> list[Gate]:
    """The review gates of an experiment's best trial (a: analysis.analysis of the experiment): the experiment still
    believes it once its tries are counted, every trial ran all its days, enough trades on enough days, and it makes
    money with its parameters moved, the costs doubled, fills of the size shown a minute late and, when it filled
    inside the spread, every leg at its bid or ask (its robustness checks, analysis.robustness). The one place they
    are judged: a backtest shows them, review requires them."""
    best = a["best"]["id"]
    dsr, pbo = a["deflated_sharpe"].get("dsr"), a["pbo"].get("pbo")
    gates = [Gate("deflated Sharpe", dsr is not None and dsr > DSR_BAR,
                  f"{dsr} in experiment {experiment_id} (bar {DSR_BAR})"),
             Gate("overfitting probability", pbo is not None and pbo < PBO_BAR, f"{pbo} (bar {PBO_BAR})")]
    exp = ledger.experiment(experiment_id)
    trials = exp["trials"]
    stopped = [t for t in trials if t["status"] == "done" and t["error"]]
    gates.append(Gate(RAN_ALL_DAYS, not stopped, "every trial ran to its last day" if not stopped else
                      f"{len(stopped)} stopped early: {stopped[0]['error']}"))
    trial = next(t for t in trials if t["id"] == best)
    trades = (trial["summary"] or {}).get("trades", 0)
    gates.append(Gate("trades", trades >= MIN_TRADES, f"{trades} (at least {MIN_TRADES})"))
    days = len({x["opened"] for x in ledger.trial_trades([best])[best]})
    gates.append(Gate("days with a trade", days >= MIN_TRADE_DAYS, f"{days} (at least {MIN_TRADE_DAYS})"))
    gates.append(_plateau(experiment_id, bool(ledger.check_trials(experiment_id, "costs_doubled"))))
    checks = [("costs doubled", "costs_doubled", "the costs doubled"),
              ("fills of the size shown, a minute late", "fills_shown_size_late",
               "fills held to the size shown and a minute late")]
    # Filled inside the spread: it must hold up at the quotes too.
    if ledger.Costs.of(exp["spec"]["costs"]).at_the_quotes() is not None:
        checks.append(("every leg at its bid or ask", "fills_at_the_quotes", "every leg at its bid or ask"))
    for name, check, what in checks:  # this trial's own: an experiment's checks may have been run for another
        nets = [t["summary"]["net"] for t in ledger.check_trials(experiment_id, check)
                if t["summary"] and t["setting"] == trial["setting"]]
        gates.append(Gate(name, bool(nets) and min(nets) > 0,
                          f"no trial with {what} (experiment_robustness)" if not nets else f"net {min(nets):,.0f}"))
    return gates


def _research(version_id: str) -> list[Gate]:
    """To review: the gates of the experiment that found it (the one whose best it is that believes it most)."""
    best = None
    # A robustness check or an optimiser's round finds nothing: the experiment of all its rounds does.
    for t in versions.version_trials(version_id):
        if t["holdout"] or t["parent"] or t["check"] or t["report"]:
            continue
        a = analysis.analysis(t["experiment_id"])
        if a.get("best", {}).get("id") != t["id"]:
            continue
        dsr = a["deflated_sharpe"].get("dsr")
        if best is None or (dsr or 0) > (best[2] or 0):
            best = (t["experiment_id"], a, dsr)
    if best is None:
        return [Gate("found by an experiment", False, "it is not the best trial of any experiment")]
    return experiment_gates(best[0], best[1])


def _plateau(experiment_id: str, checked: bool) -> Gate:
    """Each parameter the experiment varied moved 20% down and up (analysis.robustness), every one still making money:
    the setting sits on a plateau, not a peak. A moved setting that makes no trade (an entry moved past the exit) is
    outside where the strategy works, not a neighbour: it is left out, and at least one must trade."""
    moved = ledger.check_trials(experiment_id, "parameters_moved")
    if not moved:
        return Gate("parameters moved", checked, "no parameter to move" if checked
                    else "no trial with its parameters moved (experiment_robustness)")
    waiting = [t for t in moved if t["status"] != "done"]
    if waiting:
        return Gate("parameters moved", False, f"{len(waiting)} of {len(moved)} not finished")
    traded = [t for t in moved if t["summary"]["trades"] > 0]
    idle = f"; {len(moved) - len(traded)} made no trade" if len(traded) < len(moved) else ""
    if not traded:
        return Gate("parameters moved", False, f"none of {len(moved)} settings made a trade")
    nets = [t["summary"]["net"] for t in traded]
    losing = [t["setting"] for t in traded if t["summary"]["net"] <= 0]
    return Gate("parameters moved", not losing, f"{len(traded)} settings, net {min(nets):,.0f} to {max(nets):,.0f}"
                + idle + (f"; losing: {losing[0]}" if losing else ""))


def _found(version_id: str, root: str | None = None) -> dict[str, Any]:
    """The version's own trial in research (on root, when given)."""
    found = [t for t in versions.own_trials(version_id) if root is None or versions.root_of(t) == root]
    if not found:
        raise ValueError(f"the version has no trial of its own in research{' on ' + root if root else ''} yet: "
                         "backtest it first")
    return found[0]


def research_costs(version_id: str) -> ledger.Costs:
    """The costs the version was found at: what its paper days are backtested at."""
    return ledger.Costs.of(_found(version_id, versions.portfolio(version_id))["costs"])


def _band(research: list[float], n: int) -> tuple[float, float]:
    """The PREDICTED quantiles of the P&L over n days drawn from the research's days (a bootstrap, the same draws each
    time)."""
    draw = random.Random(0)
    sums = sorted(sum(draw.choices(research, k=n)) for _ in range(2000))
    return sums[int(PREDICTED[0] * len(sums))], sums[int(PREDICTED[1] * len(sums)) - 1]


def standing(version_id: str, last: int | None = None) -> dict[str, Any] | None:
    """The version's paper days (the last `last` of them, or all) against what its research days predict over as many
    days: the paper P&L (the fills' cash flow, before fees) beside the PREDICTED band of the research's P&L before fees
    (a strategy flat at each close: the same measure). None with fewer than MIN_PREDICTED_DAYS paper days."""
    v = versions.strategy_version(version_id)
    days = evidence.paper_days(v["strategy"], v["setting"], v["code"])[-last if last else 0:]
    if len(days) < MIN_PREDICTED_DAYS:
        return None
    found = _found(version_id, versions.portfolio(version_id))["id"]
    research = [d["pnl"] for d in ledger.trial_days([found]).popitem()[1]]
    if not research:
        return None
    low, high = _band(research, len(days))
    net = sum(d["paper_net"] for d in days)
    return {"days": len(days), "first": days[0]["day"], "last": days[-1]["day"], "net": round(net, 2),
            "low": round(low, 2), "high": round(high, 2), "below": net < low, "inside": low <= net <= high}


def _predicted(version_id: str) -> Gate:
    s = standing(version_id)
    if s is None:
        return Gate("inside the prediction", False, f"fewer than {MIN_PREDICTED_DAYS} paper days")
    return Gate("inside the prediction", s["inside"],
                f"{s['days']} paper days made {s['net']:+,.0f}; the research predicts {s['low']:+,.0f} to "
                f"{s['high']:+,.0f} ({PREDICTED[0]:.0%} to {PREDICTED[1]:.0%})")


def _review(version_id: str) -> list[Gate]:
    """To paper: the paper engine runs the trading core its evidence came from (as the engine itself reports it); the
    promotion report has taken it to the holdout, and it made money there."""
    tested = versions.strategy_version(version_id)["code"].get("engine")
    try:
        running = versions.paper_engine().get_status()["code"]
    except Exception as exc:
        running, why = None, f"the paper engine does not say what it was built from ({exc})"
    else:
        why = ("the same" if running == tested else
               "the paper engine runs another trading core than the one the version was tested on: restart it on "
               "this build, or register the version again and test it on the engine as it is")
    gates = [Gate("the engine it was tested on", running is not None and running == tested, why)]
    reached = [t for t in versions.version_trials(version_id) if t["holdout"]]
    if not reached:
        return gates + [Gate("holdout", False, "holdout_report has not run it on the holdout")]
    for t in reached:
        root = t["days"][0].split("/")[0]
        cut = ledger.holdout_from(root)
        held = [x["r"] for x in ledger.trial_trades([t["id"]])[t["id"]]
                if cut is not None and date.fromisoformat(x["opened"]) >= cut]
        mean = statistics.fmean(held) if held else None
        gates.append(Gate("holdout expectancy", mean is not None and mean > 0,
                          f"{len(held)} trades on {root}'s holdout (from {cut}), mean R {mean}"))
    return gates


def _paper(version_id: str) -> list[Gate]:
    """To pilot: weeks of paper days, the paper fills matching the backtest's."""
    v = versions.strategy_version(version_id)
    days = evidence.paper_days(v["strategy"], v["setting"], v["code"])
    fills = sum(d["paper_fills"] for d in days)
    unmatched = sum(d["unmatched"] for d in days)
    share = unmatched / fills if fills else None
    gap = sum(d["paper_net"] - d["backtest_net"] for d in days)
    return [Gate("paper days", len(days) >= MIN_PAPER_DAYS, f"{len(days)} (at least {MIN_PAPER_DAYS})"),
            Gate("fills matched", share is not None and share <= MAX_UNMATCHED,
                 f"{unmatched} of {fills} fills on one side only (at most {MAX_UNMATCHED:.0%})"),
            _slippage(days),
            _predicted(version_id),
            Gate("paper against backtest", True, f"paper minus backtest, all days: {gap:+,.2f}")]


def _slippage(days: list[dict[str, Any]]) -> Gate:
    """What the paper fills paid over the mids against what the backtest of the same days did at the version's costs:
    at most MAX_SLIPPAGE times as much."""
    measured = [d for d in days if d["backtest_slippage"] is not None and d["paper_slippage"] is not None]
    if not measured:
        return Gate("slippage", False, "no paper day measured what its fills paid over the mids")
    assumed = sum(d["backtest_slippage"] for d in measured)
    paid = sum(d["paper_slippage"] for d in measured)
    return Gate("slippage", paid <= MAX_SLIPPAGE * max(assumed, 0.0),
                f"paper fills paid {paid:,.2f} over the mids on {len(measured)} days, the backtest {assumed:,.2f} "
                f"(at most {MAX_SLIPPAGE:g} times)")


def _pilot(version_id: str) -> list[Gate]:
    """To live: the pilot has run its weeks."""
    v = versions.strategy_version(version_id)
    since = next((e["at"] for e in reversed(v["events"]) if e["to_stage"] == "pilot"), None)
    days = (datetime.now(timezone.utc) - datetime.fromisoformat(since)).days if since else 0
    return [Gate("pilot weeks", days >= PILOT_DAYS, f"{days} days in the pilot (at least {PILOT_DAYS})"),
            _predicted(version_id)]


GATES = {"review": _research, "paper": _review, "pilot": _paper, "live": _pilot}


def status(version_id: str) -> dict[str, Any]:
    """Where the version stands: its trials, its paper days, the gates to its next stage, and a move under way
    (pending, with why its last step failed)."""
    v = versions.strategy_version(version_id)
    stage = v["stage"]
    following = STAGES[STAGES.index(stage) + 1] if stage not in ("live", "retired") else None
    gates = GATES[following](version_id) if following else []
    return {**v, "next_stage": following, "gates": [g.__dict__ for g in gates],
            "trials": versions.version_trials(version_id),
            "paper_days": evidence.paper_days(v["strategy"], v["setting"], v["code"])}


def holdout_report(version_id: str, root: str | None = None) -> dict[str, Any]:
    """The version over its underlying's days (root's, when it was tested on several), the holdout's included, beside
    the control (RandomBuyStrategy), at the costs it was found at there: the agent's one way to the holdout, run by the
    system, once a version and recorded. A change to the strategy is a new version, and its look at the holdout is
    counted with the others in the ledger."""
    v = versions.strategy_version(version_id)
    if any(t["holdout"] for t in versions.version_trials(version_id)):
        raise ValueError("the version has had its holdout report (get_strategy_version shows it): once a version")
    root = versions.portfolio(version_id, root)
    found = _found(version_id, root)
    if (used := versions.holdout_reports(v["family"])) >= HOLDOUT_REPORTS:
        raise ValueError(f"the family {v['family']} has had {used} holdout reports, its most ({HOLDOUT_REPORTS}): "
                         "the holdout judges an idea, it does not tune one")
    cut = ledger.holdout_from(root)
    if cut is None:
        raise ValueError(f"{root} has no holdout (config.json \"holdout\")")
    days = combo.days(root, None, None, "backtest")
    if stale := uncovered_series(cut, day_of(days[-1]) or cut):
        raise ValueError("values by day stop before the holdout's last days, where a strategy reading them would "
                         f"read nothing (Strategy::daily): {', '.join(stale)}; fill them first")
    costs = ledger.Costs.of(found["costs"])
    pricing = ledger.experiment(found["experiment_id"])["spec"]["pricing"]
    runs = {batch.label(v["strategy"], v["setting"]): (v["strategy"], v["setting"]),
            batch.label(*ledger.CONTROL): ledger.CONTROL}
    with access.system():
        out = report.report(days, runs, cut, costs,
                            family=v["family"], pricing=pricing)
    return {"version_id": version_id, **out}


def uncovered_series(cut: date, last: date) -> list[str]:
    """The values by day (data/daily) kept up to the holdout (KEPT_BY_DAY values in the 30 days before it) whose last
    value is more than STALE_DAYS before the holdout's last day, each with that value's date."""
    out = []
    for path in sorted(daily_values.daily_dir().glob("*.csv")):
        dated = sorted(date.fromisoformat(d) for d in daily_values.read_series(path))
        kept = sum(cut - timedelta(days=30) <= d < cut for d in dated)
        if kept >= KEPT_BY_DAY and dated[-1] < last - timedelta(days=STALE_DAYS):
            out.append(f"{path.stem} (last {dated[-1]})")
    return out


def promote(version_id: str, to: str, approved: str = "", reason: str = "") -> dict[str, Any]:
    """Moves the version to `to`: forward one stage when its gates pass (and, where it trades, with a person's
    approval in words); back, or to retired, with a reason. Refused (ValueError) otherwise, saying why, before
    anything is done. The move is written first (versions.set_pending) and then carried out (advance)."""
    if to not in STAGES:
        raise ValueError(f"no stage {to}: {', '.join(STAGES)}")
    v = versions.strategy_version(version_id)
    stage = v["stage"]
    if to == "retired" or STAGES.index(to) < STAGES.index(stage):
        if not reason:
            raise ValueError(f"going back to {to} or retiring needs a reason")
        note = reason
    else:
        if stage == "retired" or STAGES.index(to) != STAGES.index(stage) + 1:
            raise ValueError(f"from {stage} the next stage is {STAGES[STAGES.index(stage) + 1]}, not {to}")
        failed = [g for g in GATES[to](version_id) if not g.passed]
        if failed:
            raise ValueError("not yet: " + "; ".join(f"{g.name}: {g.detail}" for g in failed))
        if to in NEEDS_APPROVAL:
            access.require(f"approving a version into {to}")
            if not approved.strip():
                raise ValueError(f"{to} trades: it needs a person's approval, in words (who and why)")
        note = approved.strip() or "gates passed"
    versions.set_pending(version_id, to, note)
    versions.advance(version_id)
    return status(version_id)


def watch() -> dict[str, Any]:
    """Every version that trades, against its prediction (standing): one whose last PREDICTION_DAYS paper days fall
    below the band goes back a stage (off the paper engine from paper); and the share of those with enough paper days
    whose paper days, all of them, fall inside it (the realization rate). The daily report's."""
    rows, back = [], []
    for v in versions.strategy_versions():
        if not versions.accounts_of(v["stage"]):
            continue
        try:
            whole, recent = standing(v["id"]), standing(v["id"], PREDICTION_DAYS)
        except ValueError as exc:  # nothing of its own in research to predict by
            rows.append({"version_id": v["id"], "strategy": v["strategy"], "stage": v["stage"], "error": str(exc)})
            continue
        rows.append({"version_id": v["id"], "strategy": v["strategy"], "stage": v["stage"], "all": whole,
                     "recent": recent})
        # Judged on a whole window, not on a few days.
        if recent is not None and recent["days"] >= PREDICTION_DAYS and recent["below"]:
            to = STAGES[STAGES.index(v["stage"]) - 1]
            reason = (f"automatic: its last {recent['days']} paper days made {recent['net']:+,.0f}, below the "
                      f"{PREDICTED[0]:.0%} of what its research predicts ({recent['low']:+,.0f})")
            promote(v["id"], to, reason=reason)
            back.append({"version_id": v["id"], "to": to, "reason": reason})
    judged = [r for r in rows if r.get("all") is not None]
    rate = sum(r["all"]["inside"] for r in judged) / len(judged) if judged else None
    return {"versions": rows, "demoted": back, "realization_rate": rate, "judged": len(judged)}
