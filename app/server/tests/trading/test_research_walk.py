"""The way from a strategy to pilot, walked whole on synthetic days with intraday momentum planted, the agent and the
person each doing only their part: the agent backtests, registers the best, asks for review, runs the holdout report
and retires the version (after the daily watch has sent it back from pilot); the person approves paper and pilot; the system puts the version on the paper engine (a
fake one here), and the daily report keeps each of its paper days against a backtest of the day. On the way, what the
agent may not do is refused."""

from __future__ import annotations

import shutil
from datetime import date, timezone

import pytest

from optra import settings
from optra.infra import access, live
from optra.research import backtest, code_identity, evidence, jobs, ledger, promotion, store, versions
from optra.services import session
from optra.data import synthetic_days
from optra.tools import ToolError, call_sync
from optra.utils import files
from optra.utils.files import repo_root

pytestmark = pytest.mark.skipif(not (repo_root() / "engine" / "build" / "entry_backtest").exists(),
                                reason="entry_backtest is not built")

STRATEGY = "SyntheticMomentumStrategy"
HOLDOUT = date(2023, 4, 3)


class PaperEngine:
    """The paper engine, as the daily report and the promotion ask it: its strategies, and fills it is told."""

    def __init__(self) -> None:
        self.strategies: dict[str, dict] = {}
        self.fills: list[dict] = []
        self.calls: list[str] = []

    def get_status(self, timeout: float | None = None) -> dict:
        return {"detail": "engine: running", "code": code_identity.engine_hash()}  # built as research's entry_backtest

    def get_strategy_classes(self) -> dict:
        return {"classes": [STRATEGY], "refused": []}

    def list_strategies(self) -> list[dict]:
        return [{"strategy_name": n, **s} for n, s in self.strategies.items()]

    def get_removed_strategies(self) -> list[str]:
        return []

    def add_strategy(self, cls: str, portfolio: str, setting: dict, name: str) -> dict:
        self.calls.append(f"add {name}")
        self.strategies[name] = {"class_name": cls, "portfolio": portfolio, "status": "added", "settings": setting}
        return {"status": "ok", "strategy_name": name}

    def init_strategy(self, name: str) -> dict:
        self.calls.append(f"init {name}")
        self.strategies[name]["status"] = "inited"
        return {"status": "ok"}

    def start_strategy(self, name: str) -> dict:
        self.calls.append(f"start {name}")
        self.strategies[name]["status"] = "running"
        return {"status": "ok"}

    def stop_strategy(self, name: str) -> dict:
        self.calls.append(f"stop {name}")
        self.strategies[name]["status"] = "inited"
        return {"status": "ok"}

    def remove_strategy(self, name: str) -> dict:
        self.calls.append(f"remove {name}")
        del self.strategies[name]
        return {"status": "ok", "removed": True}

    def get_strategy_holdings(self) -> dict:
        return {"holdings": {}}

    def get_orders_and_fills(self, strategy: str | None, limit: int) -> dict:
        return {"records": [f for f in self.fills if strategy in (None, f["strategy_name"])]}

    def get_risk(self) -> dict:
        return {"limits": [], "halted": False, "refused": 0, "recent_refusals": []}

    def get_account(self) -> dict:
        return {"broker_check": "ok", "broker_check_problem": ""}


@pytest.fixture
def world(tmp_path, monkeypatch, own_strategy_dirs):
    model = synthetic_days.Model(momentum=2.0, expiries=1, strikes=10)  # a plain edge: this walks the way, not the odds
    synthetic_days.generate(model, date(2023, 1, 2), date(2023, 6, 30), tmp_path / "data", "SPY", False)
    monkeypatch.setattr(files, "data_root", lambda: tmp_path / "data")
    monkeypatch.setattr(store, "_db", None)
    monkeypatch.setattr(settings, "home", lambda: tmp_path / "home")
    monkeypatch.setattr(settings, "config", lambda: {"holdout": {"SPY": HOLDOUT.isoformat()}})
    monkeypatch.setattr(promotion, "MIN_TRADES", 20)  # a few months of days, not the 300 trades of real research
    monkeypatch.setattr(promotion, "MIN_TRADE_DAYS", 20)
    monkeypatch.setattr(promotion, "HOLDOUT_REPORTS", 1)
    monkeypatch.setattr(promotion, "PREDICTED", (0.0, 1.0))  # the way, not the odds: any paper month is inside
    engine = PaperEngine()
    monkeypatch.setattr(live, "client", lambda account="paper": engine)
    return tmp_path, engine


def refused(name: str, status: int, **arguments) -> str:
    with pytest.raises(ToolError) as e:
        call_sync(name, arguments)
    assert e.value.status == status, e.value.message
    return e.value.message


def test_a_strategy_walks_from_a_backtest_to_pilot_and_the_agent_only_does_its_part(world):
    tmp_path, engine = world
    assert jobs.is_here()

    # The agent: a sweep before the holdout; the best beats the control and clears the statistics.
    out = backtest.backtest(strategy=STRATEGY, setting=None,
                            space={"signal_minutes": [15, 30], "entry_minute": [330, 360]}, search={"kind": "grid"},
                            family="late momentum", symbol="SPY", start=None, end=None,
                            costs=ledger.Costs())
    assert out["last_day"] < HOLDOUT.strftime("%Y%m%d")
    assert out["beats_control_by_r"] > 0 and out["deflated_sharpe"] > promotion.DSR_BAR, out["review"]
    assert out["tries"] == len(out["trials"])  # what was tried by then; the control is not counted
    assert ledger.tries_by(ledger.experiments(1)[0]["id"]) >= len(out["trials"]) + 3  # the robustness checks are

    # Why the best lost on its worst days: run again as it ran, its fills and what it logged, checked against the
    # ledger, and not a try more.
    tries = ledger.tries_by(ledger.experiments(1)[0]["id"])
    replay = call_sync("replay_trial", {"trial_id": out["best"]["id"], "worst": 3})
    shown = replay["result_details"]["days"]
    assert len(shown) == 3 and replay["result_summary"]["differs_from_ledger"] == []
    assert all(d["day"] < HOLDOUT.isoformat() for d in shown)
    assert any(d["fills"] for d in shown) and any(r["msg"].startswith("Fill ") for d in shown for r in d["log"])
    assert ledger.tries_by(ledger.experiments(1)[0]["id"]) == tries

    # What it may not do: reach the holdout, raise its budget, approve.
    assert "holdout" in refused("backtest_report", 400, symbol="SPY", strategies=[STRATEGY])
    refused("set_family_budget", 403, family="late momentum", budget=1000)
    with pytest.raises(ValueError, match="the control's"):
        ledger.submit(family="control", strategy=STRATEGY, days=[str(f) for f in ledger.experiment_days(
            out["experiment_id"])], settings_list=[{}])

    version = call_sync("register_strategy_version", {
        "trial_id": out["best"]["id"], "family": "late momentum", "hypothesis": "The morning's move carries on late.",
        "edge": "Planted: the synthetic days' momentum.",
        "fails_when": "Never, on these days."})["result_summary"]["id"]
    review = call_sync("promote_strategy_version", {"version_id": version, "to": "review"})["result_summary"]
    assert review["stage"] == "review" and review["next_stage"] == "paper"
    refused("promote_strategy_version", 403, version_id=version, to="paper")
    refused("approve_strategy_version", 403, version_id=version, to="paper", approved="the agent itself")

    # The holdout, through the version, once.
    report = call_sync("holdout_report", {"version_id": version})["result_summary"]
    assert [p["period"] for p in report["periods"]][1].startswith("holdout")
    refused("holdout_report", 409, version_id=version)
    other = next(t for t in out["trials"] if t["id"] != out["best"]["id"])
    second = call_sync("register_strategy_version", {
        "trial_id": other["id"], "family": "late momentum", "hypothesis": "The same, another setting.",
        "edge": "Planted.", "fails_when": "Never."})["result_summary"]["id"]
    with pytest.raises(ToolError, match="holdout reports, its most"):  # a family's look at the holdout is counted
        call_sync("holdout_report", {"version_id": second})
    assert evidence.holdout_uses(f"{STRATEGY}:" + ",".join(
        f"{k}={float(x)!r}" for k, x in sorted(versions.strategy_version(version)["setting"].items()))) == 1

    # The person approves paper: started on the paper engine.
    with access.system():
        paper = call_sync("approve_strategy_version", {"version_id": version, "to": "paper",
                                                       "approved": "the owner: the walk"})["result_summary"]
    name = promotion.engine_name(version, versions.strategy_version(version))
    assert paper["stage"] == "paper"
    assert engine.calls == [f"add {name}", f"init {name}", f"start {name}"]
    assert engine.strategies[name]["portfolio"] == "SPY"
    assert not (tmp_path / "paper").exists()  # a demo strategy: every engine loads it, nothing to install

    # Twenty paper days, each recorded and compared by the daily report; the paper fills here are the backtest's.
    days = sorted(p for p in (tmp_path / "data" / "SPY").rglob("*.parquet") if p.stem >= HOLDOUT.strftime("%Y%m%d"))
    v = versions.strategy_version(version)
    for f in days[:promotion.MIN_PAPER_DAYS]:
        day = date(int(f.stem[:4]), int(f.stem[4:6]), int(f.stem[6:]))
        recorded = tmp_path / "data" / "recorded" / "SPY" / f.parent.name  # the recorder's days, put there here
        recorded.mkdir(parents=True, exist_ok=True)
        shutil.copy(f, recorded / f.name)
        fills, _ = session.backtest_fills(tmp_path / "data" / "recorded", "SPY", STRATEGY, v["setting"],
                                          ledger.Costs(), day)
        for fill in fills:
            engine.fills.append({"record_type": "Fill", "strategy_name": name, "symbol": fill["symbol"],
                                 "side": fill["side"], "price": fill["price"], "quantity": fill["quantity"],
                                 "timestamp": fill["time"].astimezone(timezone.utc).strftime("%Y-%m-%d %H:%M:%S")})
        session.report(engine, day, None, send=False)
    kept = evidence.paper_days(STRATEGY, v["setting"], v["code"])
    assert len(kept) == promotion.MIN_PAPER_DAYS and all(d["unmatched"] == 0 for d in kept)
    assert all(d["paper_slippage"] == pytest.approx(d["backtest_slippage"]) for d in kept)  # the backtest's fills
    gates = {g["name"]: g for g in promotion.status(version)["gates"]}
    assert gates["slippage"]["passed"] and gates["inside the prediction"]["passed"], gates

    with access.system():
        pilot = call_sync("approve_strategy_version", {"version_id": version, "to": "pilot",
                                                       "approved": "the owner: the walk"})["result_summary"]
    assert pilot["stage"] == "pilot" and name in engine.strategies

    # Weeks far below what the research predicts: the daily watch sends it back a stage, still on the paper engine.
    for i, f in enumerate(days[promotion.MIN_PAPER_DAYS:promotion.MIN_PAPER_DAYS + promotion.PREDICTION_DAYS]):
        evidence.record_paper_day(name, STRATEGY, v["setting"], f"{f.stem[:4]}-{f.stem[4:6]}-{f.stem[6:]}", -1e6, 0.0,
                                2, 2, 0, kept[0]["code"], 1.0, 1.0)
    watched = promotion.watch()
    assert [(d["version_id"], d["to"]) for d in watched["demoted"]] == [(version, "paper")]
    assert versions.strategy_version(version)["stage"] == "paper" and name in engine.strategies
    assert watched["realization_rate"] == 0.0

    # The agent retires it: off the paper engine.
    retired = call_sync("promote_strategy_version", {"version_id": version, "to": "retired",
                                                     "reason": "the walk is over"})["result_summary"]
    assert retired["stage"] == "retired" and engine.calls[-2:] == [f"stop {name}", f"remove {name}"]
