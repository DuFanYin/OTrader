"""Strategy versions as tools: register a version (a class, its setting frozen, the code it is, with its card), see its
evidence and the gates to its next stage, and move it, forward on evidence or back with a reason. See
research/promotion.py."""

from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, ConfigDict, Field

from optra.research import ledger, promotion, versions
from optra.tools.base import Result, Tool, ToolError
from optra.tools.experiments import EXAMPLE_TRIAL


class Input(BaseModel):
    model_config = ConfigDict(extra="forbid")


class RegisterRequest(Input):
    trial_id: str = Field(..., description="The trial whose strategy and setting become the version (get_experiment "
                                           "lists them); the version is the code as it is now.")
    family: str = Field(..., description="The hypothesis it belongs to, as its experiments named it.")
    hypothesis: str = Field(..., description="What it bets on, in a sentence.")
    edge: str = Field(..., description="Whose money it makes, and why they let it: the source of the return.")
    fails_when: str = Field(..., description="The market or the evidence that would mean the edge is gone.")


class VersionRequest(Input):
    version_id: str = Field(..., description="A strategy version's id (register_strategy_version returns it).")


class ListRequest(Input):
    stage: Literal["research", "review", "paper", "pilot", "live", "retired"] | None = Field(
        None, description="Only the versions at this stage.")


class PromoteRequest(VersionRequest):
    to: Literal["research", "review", "paper", "pilot", "retired"] = Field(
        ..., description="review (its gates must pass), or an earlier stage, or retired. Into paper, pilot and live a "
                         "person approves it (approve_strategy_version).")
    reason: str = Field("", description="Back to an earlier stage, or retired: why.")


class ApproveRequest(VersionRequest):
    to: Literal["paper", "pilot", "live"] = Field(..., description="The next stage, where it trades.")
    approved: str = Field(..., min_length=3, description="Who approves it and why.")


def register_strategy_version(p: RegisterRequest) -> Result:
    try:
        trial = ledger.trial(p.trial_id)
    except KeyError as exc:
        raise ToolError(f"no trial {p.trial_id}", 404) from exc
    version_id = versions.register_version(trial["strategy"], trial["setting"], p.family,
                                         {"hypothesis": p.hypothesis, "edge": p.edge, "fails_when": p.fails_when})
    return Result(summary=_summary(promotion.status(version_id)))


def _summary(e: dict) -> dict:
    return {k: e[k] for k in ("id", "strategy", "setting", "family", "stage", "next_stage", "gates", "card", "pending",
                              "pending_error")} | {
        "trials": len(e["trials"]), "paper_days": len(e["paper_days"])}


def get_strategy_version(p: VersionRequest) -> Result:
    try:
        e = promotion.status(p.version_id)
    except KeyError as exc:
        raise ToolError(f"no strategy version {p.version_id}", 404) from exc
    return Result(summary=_summary(e), details={"trials": e["trials"], "paper_days": e["paper_days"],
                                                "events": e["events"], "code": e["code"]})


def list_strategy_versions(p: ListRequest) -> Result:
    return Result(summary={"versions": versions.strategy_versions(p.stage)})


def promote_strategy_version(p: PromoteRequest) -> Result:
    v = _version(p.version_id)
    if p.to != "retired" and promotion.STAGES.index(p.to) > promotion.STAGES.index(v["stage"]) and p.to != "review":
        raise ToolError(f"into {p.to} a person approves it (approve_strategy_version, on the page)", 403)
    return _move(p.version_id, p.to, "", p.reason)


def approve_strategy_version(p: ApproveRequest) -> Result:
    return _move(p.version_id, p.to, p.approved, "")


def holdout_report(p: VersionRequest) -> Result:
    _version(p.version_id)
    try:
        out = promotion.holdout_report(p.version_id)
    except ValueError as exc:
        raise ToolError(str(exc), 409) from exc
    return Result(summary={"version_id": p.version_id, "days": out["days"], "header": out["header"],
                           "periods": [{"period": x["label"], "rows": x["rows"]} for x in out["periods"]]},
                  details={"trades": out["trades"], "markdown": out["markdown"], "errors": out["errors"]},
                  notes=["Once a version: the gate to paper reads its holdout trades from the ledger."])


def _version(version_id: str) -> dict:
    try:
        return versions.strategy_version(version_id)
    except KeyError as exc:
        raise ToolError(f"no strategy version {version_id}", 404) from exc


def _move(version_id: str, to: str, approved: str, reason: str) -> Result:
    try:
        e = promotion.promote(version_id, to, approved, reason)
    except KeyError as exc:
        raise ToolError(f"no strategy version {version_id}", 404) from exc
    except ValueError as exc:
        raise ToolError(str(exc), 409) from exc
    return Result(summary=_summary(e))


VERSION = ("Strategy versions: a strategy class with its setting frozen and the code it is, moved from research to "
           "live one stage at a time on the evidence in the ledger. ")
EXAMPLE_VERSION = "fedcba9876543210"  # the examples' stand-in for a real version

TOOLS = [
    Tool("register_strategy_version", "Register a strategy version",
         VERSION + "Makes a trial's strategy and setting, with the code as it is now, a version under its family, with "
         "its card (hypothesis, where the edge comes from, when it fails). It starts in research. Returns it with the "
         "gates to review.",
         RegisterRequest, register_strategy_version, "/strategy_versions", engine=False, record=False,
         example={"trial_id": EXAMPLE_TRIAL, "family": "0DTE straddle timing",
                  "hypothesis": "A move late in the day carries on into the close.",
                  "edge": "Hedgers forced to chase gamma into the close.",
                  "fails_when": "Moves late in the day revert as often as they carry on."}),
    Tool("get_strategy_version", "Look up a strategy version",
         VERSION + "A version with its evidence: its trials (in experiments and the promotion report), its paper days, "
         "its stage changes, and each gate to its next stage, passed or not, with why.",
         VersionRequest, get_strategy_version, "/strategy_versions/{version_id}", method="GET", engine=False,
         record=False, example={"version_id": EXAMPLE_VERSION}),
    Tool("list_strategy_versions", "List strategy versions",
         VERSION + "Every version, or those at one stage, newest first.",
         ListRequest, list_strategy_versions, "/strategy_versions", method="GET", engine=False, record=False,
         example={}),
    Tool("holdout_report", "Holdout report of a version",
         VERSION + "The one way to the holdout: the system runs the version (its setting frozen, at the costs it was "
         "found at) over its underlying's days, the holdout's included, beside the control, and reports the research "
         "days and the holdout apart (trades, win rate, expectancy in R with its t, net, drawdown, the gap to the "
         "control). Once a version, recorded; the gate to paper reads it.",
         VersionRequest, holdout_report, "/strategy_versions/{version_id}/holdout_report", cost="heavy", engine=False,
         record=False, example={"version_id": EXAMPLE_VERSION}),
    Tool("promote_strategy_version", "Move a strategy version",
         VERSION + "Asks for review (its gates must pass: found by an experiment with a deflated Sharpe above 0.95 and "
         "an overfitting probability below 0.2 once every trial in the ledger is counted, 300 trades, still positive "
         "with the costs doubled and with fills held to the size shown a minute late), or moves it back, or retires "
         "it, with a reason (a version on the paper engine is stopped and taken off). Into paper, pilot and live a "
         "person approves it. Refused, saying which gate is not met.",
         PromoteRequest, promote_strategy_version, "/strategy_versions/{version_id}/stage", engine=False, record=False,
         example={"version_id": EXAMPLE_VERSION, "to": "retired", "reason": "the example"}),
    Tool("approve_strategy_version", "Approve a strategy version",
         VERSION + "The person's: moves a version into the next stage where it trades, when its gates pass. paper: "
         "positive on the holdout (holdout_report), on the engine it was tested on; it is then installed for the paper "
         "account and started on the paper engine as <class>_<version id>, and the daily report compares each of its "
         "days there with a backtest of the day. pilot: 20 paper days with the fills matching the backtest's. live: 8 "
         "weeks of pilot.",
         ApproveRequest, approve_strategy_version, "/strategy_versions/{version_id}/approve", engine=False,
         record=False, person=True,
         example={"version_id": EXAMPLE_VERSION, "to": "paper", "approved": "the owner: the example"}),
]
