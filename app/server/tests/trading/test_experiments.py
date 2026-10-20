"""Experiments end to end on synthetic days: an experiment registers its trials before they run and counts them in
its family, a worker runs them through entry_backtest --batch, the same trial is never run twice, the family's
budget and the holdout refuse what they should, and the analysis comes back from the numerics library."""

from __future__ import annotations

import json
import math
from datetime import date
from pathlib import Path

import numpy as np
import pytest

from optra.infra import numerics
from optra.research import analysis, batch, evidence, jobs, ledger, runner, space, store, strategies, versions
from optra.data import synthetic_days
from optra.utils import files
from optra.utils.files import repo_root

pytestmark = pytest.mark.skipif(not (repo_root() / "engine" / "build" / "entry_backtest").exists(),
                                reason="entry_backtest is not built")


@pytest.fixture
def days(tmp_path, monkeypatch):
    model = synthetic_days.Model(implied_vol=0.2)
    synthetic_days.generate(model, date(2025, 3, 3), date(2025, 3, 7), tmp_path / "backtest", "SPY", False)
    monkeypatch.setattr(files, "data_root", lambda: tmp_path)
    monkeypatch.setattr(store, "_db", None)
    return sorted(str(p) for p in (tmp_path / "backtest" / "SPY").rglob("*.parquet"))


def test_the_ledger_names_a_backtest_day_as_it_did_before_data_backtest(tmp_path, monkeypatch):
    monkeypatch.setattr(files, "data_root", lambda: tmp_path)
    day = tmp_path / "backtest" / "SPY" / "SPY-2025-03" / "20250303.parquet"
    recorded = tmp_path / "recorded" / "SPY" / "SPY-2025-03" / "20250303.parquet"
    for f in (day, recorded):
        f.parent.mkdir(parents=True)
        f.write_bytes(b"")
    assert files.ledger_name(day) == "SPY/SPY-2025-03/20250303.parquet"  # older experiments' days and hashes
    assert files.ledger_name(recorded) == "recorded/SPY/SPY-2025-03/20250303.parquet"
    assert [files.from_ledger_name(files.ledger_name(f)) for f in (day, recorded)] == [day, recorded]


def test_a_days_values_come_back_with_its_row():
    rows = runner.rows({"date": [20250303, 20250304], "pnl": [1.0, 2.0], "attribution": {"delta": [0.5, 1.0]},
                        "values": {"var_day": [1e-4, None], "returns": [389, 389]}})
    assert rows[0]["values"] == {"var_day": 1e-4, "returns": 389}
    assert rows[1]["values"] == {"returns": 389}  # a day it wrote no var_day: none
    assert rows[0]["delta"] == 0.5 and rows[1]["pnl"] == 2.0


def test_the_other_underlying_has_the_same_dates_before_its_holdout(tmp_path, monkeypatch):
    for symbol in ("SPY", "SPXW"):
        synthetic_days.generate(synthetic_days.Model(), date(2025, 3, 3), date(2025, 3, 5), tmp_path / "backtest",
                                symbol, False)
    monkeypatch.setattr(files, "data_root", lambda: tmp_path)
    monkeypatch.setattr(ledger, "holdout_from", lambda root: date(2025, 3, 5) if root == "SPXW" else None)
    spy = sorted((tmp_path / "backtest" / "SPY").rglob("*.parquet"))
    other, found = analysis.other_underlying_days(spy)
    assert other == "SPXW" and [f.stem for f in found] == ["20250303", "20250304"]
    assert analysis.other_underlying_days(found)[0] == "SPY"


def run_queued() -> int:
    """What a worker does, in-process: lease, run the batch, report, until nothing waits. How many jobs."""
    ran = 0
    while (job := jobs.lease("test")) is not None:
        assert jobs.complete(job["job_id"], "test", runner.run_batch(job["batch"], threads=2, days_ahead=3))["accepted"]
        ran += 1
    return ran


def alone(day_files, strategy, setting, costs):
    """One class and setting over the days in a batch of its own, outside the ledger: its round trips."""
    trial = {"id": "alone", "strategy": strategy, "setting": setting, "costs": costs.model_dump()}
    batch_ = {"files": [str(f) for f in day_files], "risk_free_rate": 0.05, "iv_price_mode": "mid", "trials": [trial]}
    return batch.trades_of(jobs.run(batch_, jobs.PRIORITY_REPORT)[1][0])[0]


def submit(days, **kw):
    args = {"family": "random buys", "strategy": "RandomBuyStrategy", "days": days,
            "space": {"seed": [1, 2, 3], "hold_minutes": [30, 60]}}
    return ledger.submit(**{**args, **kw})


def test_an_experiment_runs_each_setting_once_and_keeps_every_day(days):
    out = submit(days)
    assert out["trials"] == 6 and out["queued"] == 6 and out["reused"] == 0
    assert ledger.families()["random buys"] == {"budget": 200, "used": 6, "done": 0}
    assert run_queued() == 1  # one job: the six trials in one batch, each day loaded once
    exp = ledger.experiment(out["experiment_id"])
    assert [t["status"] for t in exp["trials"]] == ["done"] * 6
    ids, day_list, values = ledger.matrix(out["experiment_id"])
    assert len(ids) == 6 and len(day_list) == len(days)
    assert all(not math.isnan(v) for row in values for v in row)

    # The same settings again: nothing runs, the trials are the ones already done, the family counts them once.
    again = submit(days)
    assert again["queued"] == 0 and again["reused"] == 6
    assert run_queued() == 0
    assert ledger.families()["random buys"]["used"] == 6


def test_a_trial_in_a_batch_is_the_backtest_run_alone(days):
    out = submit(days, space={"seed": [4], "hold_minutes": [45]})
    run_queued()
    trial_id = ledger.trial_ids(out["experiment_id"])[0]
    run_alone = alone([files.data_root() / d for d in days], "RandomBuyStrategy", {"seed": 4, "hold_minutes": 45},
                      ledger.Costs())
    trades = ledger.trial_trades([trial_id])[trial_id]
    assert [round(t["net"], 6) for t in trades] == [round(t.net, 6) for t in run_alone]


def test_the_family_budget_refuses_an_experiment_whole(days):
    submit(days)
    ledger.set_budget("random buys", 8)
    with pytest.raises(ValueError, match="has tried 6 of its 8"):
        submit(days, space={"seed": [5, 6, 7]})
    assert ledger.families()["random buys"]["used"] == 6  # nothing of it registered


def test_experiments_submitted_together_run_as_one_batch_and_register_all_or_none(days):
    # A worker waiting takes a job the moment it is queued: experiments on the same days submitted one after another
    # would each run alone, each reading the days; together, they are one job.
    a, b = ledger.submit_together([
        {"family": "random buys", "strategy": "RandomBuyStrategy", "days": days, "settings_list": [{"seed": 1}]},
        {"family": ledger.CONTROL_FAMILY, "strategy": ledger.CONTROL[0], "days": days,
         "settings_list": [ledger.CONTROL[1]]}])
    assert run_queued() == 1
    for out in (a, b):
        assert [t["status"] for t in ledger.experiment(out["experiment_id"])["trials"]] == ["done"]

    ledger.set_budget("random buys", 2)
    with pytest.raises(ValueError, match="has tried 1 of its 2"):
        ledger.submit_together([
            {"family": "other buys", "strategy": "RandomBuyStrategy", "days": days, "settings_list": [{"seed": 9}]},
            {"family": "random buys", "strategy": "RandomBuyStrategy", "days": days,
             "settings_list": [{"seed": 2}, {"seed": 3}]}])
    assert "other buys" not in ledger.families() or ledger.families()["other buys"]["used"] == 0


def test_holdout_days_cannot_be_in_an_experiment(days, monkeypatch):
    monkeypatch.setattr(ledger.settings, "config", lambda: {"holdout": {"SPY": "2025-03-06"}})
    with pytest.raises(ValueError, match="holdout"):
        submit(days)


def test_a_cancelled_trial_still_counts(days):
    out = submit(days)
    ledger.cancel(out["experiment_id"])
    assert run_queued() == 0
    assert ledger.families()["random buys"]["used"] == 6
    assert {t["status"] for t in ledger.experiment(out["experiment_id"])["trials"]} == {"cancelled"}


def test_cancelling_an_experiment_stops_its_running_job(days):
    out = submit(days)
    job = jobs.lease("w")
    assert {t["status"] for t in ledger.experiment(out["experiment_id"])["trials"]} == {"running"}
    assert ledger.cancel(out["experiment_id"])["cancelled_jobs"] == 1
    assert jobs.renew(job["job_id"], "w") == "cancelled"  # the worker stops the run there
    assert {t["status"] for t in ledger.experiment(out["experiment_id"])["trials"]} == {"cancelled"}


def test_a_job_whose_worker_vanished_puts_its_trials_back_in_the_queue(days):
    out = submit(days)
    job = jobs.lease("w")
    store.db().execute("UPDATE jobs SET lease_until = 0 WHERE id = ?", (job["job_id"],))
    assert jobs.lease("w2")["job_id"] == job["job_id"]  # offered again: its trials queued, then running again
    assert ledger.experiment(out["experiment_id"])["jobs"] == {"leased": 1}


def test_random_and_latin_searches_sample_inside_the_ranges(days):
    for kind in ("random", "latin"):
        settings = space.expand("RandomBuyStrategy", {"hold_minutes": {"range": [10, 60]},
                                                            "seed": [1, 2]}, {"kind": kind, "n": 8, "seed": 3})
        assert 1 < len(settings) <= 8
        assert all(10 <= s["hold_minutes"] <= 60 and s["hold_minutes"] == round(s["hold_minutes"]) for s in settings)


def test_the_analysis_counts_the_family_tries(days):
    out = submit(days)
    run_queued()
    if not numerics.is_engine_available():
        pytest.skip("the numerics library is not built")
    a = analysis.analysis(out["experiment_id"], train=2, test=1)
    assert a["family_tries"] == 6 and a["trials"] == 6
    assert a["deflated_sharpe"]["tries"] == 6
    assert len(a["walk_forward"]["windows"]) == len(days) - 2
    assert set(a["by_regime"]) >= {"up", "down"} or "why" in a["by_regime"]


def test_the_analyses_kept_are_the_ones_used_last(days, monkeypatch):
    out = submit(days)
    run_queued()
    if not numerics.is_engine_available():
        pytest.skip("the numerics library is not built")
    monkeypatch.setattr(analysis, "_analyses", type(analysis._analyses)())
    monkeypatch.setattr(analysis, "ANALYSES_KEPT", 2)
    first = analysis.analysis(out["experiment_id"], train=2, test=1)
    analysis.analysis(out["experiment_id"], train=3, test=1)
    first["family"] = "changed by the caller"
    assert analysis.analysis(out["experiment_id"], train=2, test=1)["family"] != first["family"]  # a copy
    analysis.analysis(out["experiment_id"], train=4, test=1)  # the train=3 one goes: used longest ago
    assert [k[3] for k in analysis._analyses] == [2, 4]


def test_selection_statistics_tell_noise_from_an_edge():
    if not numerics.is_engine_available():
        pytest.skip("the numerics library is not built")
    import random

    rng = random.Random(1)
    noise = [[rng.gauss(0, 1) for _ in range(160)] for _ in range(40)]
    out = numerics.selection(np.array(noise), 40)
    assert out["deflated_sharpe"]["dsr"] < 0.95
    assert 0.25 < out["pbo"]["pbo"] < 0.85  # noise: the best in sample is anywhere out of sample
    edge = [row[:] for row in noise]
    edge[11] = [x + 0.5 for x in edge[11]]
    out = numerics.selection(np.array(edge), 40)
    assert out["deflated_sharpe"]["best"] == 11 and out["deflated_sharpe"]["dsr"] > 0.95
    assert out["pbo"]["pbo"] < 0.05
    assert json.dumps(out)  # plain JSON


def test_the_report_counts_its_strategies_as_trials_and_reuses_them(days):
    from optra.research import report

    runs = {f"RandomBuyStrategy:seed={s}": ("RandomBuyStrategy", {"seed": s}) for s in (1, 2)}
    first = report.report(days, runs, None, ledger.Costs())
    assert ledger.families()["control"]["used"] == 2  # the control, whatever the report
    again = report.report(days, runs, None, ledger.Costs())
    assert again["periods"] == first["periods"]  # read back from the ledger, nothing run again
    assert ledger.families()["control"]["used"] == 2
    run_alone = alone([files.data_root() / d for d in days], "RandomBuyStrategy", {"seed": 1}, ledger.Costs())
    assert [t["net"] for t in first["trades"]["RandomBuyStrategy:seed=1"]] == [round(t.net, 2) for t in run_alone]


def test_the_page_backtest_runs_through_the_batch_with_its_chart(days):
    from optra.services import backtest

    out = backtest.run_backtest({"parquet": "SPY", "strategy": "RandomBuyStrategy", "start_date": "2025-03-03",
                                 "end_date": "2025-03-07", "costs": {"fee_scale": 1.0}, "strategy_setting": {"seed": 3}})
    assert out["status"] == "ok" and out["chart"]["pnl"] and len(out["days"]["date"]) == len(days)
    assert out["day_files"]["date"] == out["days"]["date"] and all(out["day_files"]["bars"])
    assert backtest.cancel_backtest()["message"] == "no active backtest"


def test_the_engine_refuses_a_cost_it_does_not_have(days):
    with pytest.raises(ValueError, match="No cost 'slippage_bps'"):
        runner.run_trials([files.data_root() / d for d in days],
                          [{"id": "t", "strategy": "RandomBuyStrategy", "costs": {"slippage_bps": 5}}])


def test_a_setting_written_with_its_defaults_is_the_same_trial(days):
    first = submit(days, space={"seed": [1]})
    defaults = {p["name"]: p["default"] for p in strategies.strategy_classes()["RandomBuyStrategy"]}
    again = submit(days, space={**{k: v for k, v in defaults.items() if k != "seed"}, "seed": [1]})
    assert again["trial_ids"] == first["trial_ids"] and again["queued"] == 0


def test_a_version_moves_only_on_its_evidence(days):
    from optra.research import promotion

    out = submit(days, space={"seed": [1, 2]})
    run_queued()
    version = versions.register_version("RandomBuyStrategy", ledger.trial(out["trial_ids"][0])["setting"],
                                      "random buys", {"hypothesis": "none: the control"})
    e = promotion.status(version)
    assert e["stage"] == "research" and e["next_stage"] == "review" and len(e["trials"]) == 1
    assert not all(g["passed"] for g in e["gates"])  # ten days are not 300 trades
    with pytest.raises(ValueError, match="not yet"):
        promotion.promote(version, "review")
    with pytest.raises(ValueError, match="next stage is review"):
        promotion.promote(version, "paper", approved="me")
    with pytest.raises(ValueError, match="reason"):
        promotion.promote(version, "retired")
    assert promotion.promote(version, "retired", reason="the control")["stage"] == "retired"
    assert [ev["to_stage"] for ev in versions.strategy_version(version)["events"]] == ["retired"]


def test_a_paper_day_joins_its_version_by_class_setting_and_code(days):
    setting = {p["name"]: p["default"] for p in strategies.strategy_classes()["RandomBuyStrategy"]}
    code = {"core": "c", "engine": "e", "strategy": "s"}
    evidence.record_paper_day("RandomBuyStrategy_SPY", "RandomBuyStrategy", {"seed": setting["seed"]}, "2025-03-03",
                            10.0, 12.0, 2, 2, 0, {"engine": "e", "strategy": "s"})
    assert len(evidence.paper_days("RandomBuyStrategy", setting, code)) == 1  # the same setting, written in full
    assert evidence.paper_days("RandomBuyStrategy", setting, {**code, "strategy": "another"}) == []


def test_research_refuses_an_engine_whose_build_does_not_say_what_from(days, monkeypatch, tmp_path):
    from optra.research import code_identity

    old = tmp_path / "entry_backtest"  # a program with no build record beside it
    old.write_text("")
    monkeypatch.setattr(code_identity, "entry_backtest", lambda: old)
    with pytest.raises(ValueError, match="not built"):
        code_identity.check_build()


def test_research_refuses_a_strategy_library_whose_files_changed_since_it_was_built(tmp_path):
    import hashlib

    from optra.research import code_identity

    source, library = tmp_path / "probe.cpp", tmp_path / "probe.so"
    source.write_text("built")
    library.write_text("")
    Path(f"{library}.code").write_text(json.dumps({"strategy": "s", "files": [
        {"path": str(source), "sha256": hashlib.sha256(b"built").hexdigest()}]}))
    code_identity.check_build({"source": str(source), "library": str(library)})
    source.write_text("changed")
    with pytest.raises(ValueError, match=r"probe\.so was built before .*probe\.cpp changed.*build_strategies"):
        code_identity.check_build({"source": str(source), "library": str(library)})


def test_the_strategies_are_libraries_the_listing_names(days):
    built = strategies.built_strategies()
    random_buy = next(c for c in built["strategies"] if c["name"] == "RandomBuyStrategy")
    assert random_buy["library"].endswith("strategies/random_buy.so")
    assert random_buy["source"].endswith("strategy/demo/random_buy.cpp")
    assert built["refused"] == []


def test_how_orders_fill_is_part_of_the_trial(days):
    quote = submit(days, space={"seed": [1]})
    strict = submit(days, space={"seed": [1]}, costs={"fill": "shown_size", "delay_bars": 1})
    assert strict["trial_ids"] != quote["trial_ids"] and strict["queued"] == 1
    run_queued()
    trial = ledger.trial(strict["trial_ids"][0])
    assert trial["status"] == "done" and trial["costs"]["fill"] == "shown_size" and trial["costs"]["delay_bars"] == 1
    with pytest.raises(ValueError, match="'quote' or 'shown_size'"):
        submit(days, costs={"fill": "mid"})


def test_the_robustness_checks_are_experiments_of_the_same_family(days):
    out = submit(days, space={"seed": [1, 2]})
    run_queued()
    checks = analysis.robustness(out["experiment_id"])
    assert checks["costs_doubled"]["trials"] == 1 and checks["fills_shown_size_late"]["trials"] == 1
    run_queued()
    strict = ledger.trial(checks["fills_shown_size_late"]["trial_ids"][0])
    assert strict["status"] == "done" and strict["costs"]["fill"] == "shown_size"
    assert ledger.families()["random buys"]["used"] == 2 + 2 + checks["parameters_moved"]["trials"] \
        if checks["parameters_moved"] else 4


def test_a_robustness_check_is_not_where_a_version_was_found(days):
    from optra.research import promotion

    out = submit(days, space={"seed": [1, 2, 3], "hold_minutes": [60]})  # hold_minutes moves 20%; a seed does not
    run_queued()
    best = analysis.analysis(out["experiment_id"])["best"]["id"]
    analysis.robustness(out["experiment_id"], best)
    run_queued()
    version = versions.register_version("RandomBuyStrategy", ledger.trial(best)["setting"], "random buys",
                                      {"hypothesis": "-", "edge": "-", "fails_when": "-"})
    gates = {g.name: g for g in promotion._research(version)}
    assert out["experiment_id"] in gates["deflated Sharpe"].detail  # the search that found it, not a check of it
    # Each check found as the check it was registered as, whatever its costs.
    assert gates["costs doubled"].detail.startswith("net ")
    assert gates["fills of the size shown, a minute late"].detail.startswith("net ")
    # The settings around it, each parameter moved: all of them must make money; and the days it traded on counted.
    moved = ledger.check_trials(out["experiment_id"], "parameters_moved")
    traded = [t for t in moved if t["summary"]["trades"] > 0]
    assert moved and gates["parameters moved"].passed == (bool(traded) and all(t["summary"]["net"] > 0 for t in traded))
    assert gates["days with a trade"].detail.startswith(str(len({t["opened"] for t in ledger.trial_trades([best])[best]})))


def test_a_moved_setting_that_makes_no_trade_is_not_a_neighbour(monkeypatch):
    from optra.research import promotion

    def trial(net: float, trades: int) -> dict:
        return {"status": "done", "setting": {"entry_minute": 389}, "summary": {"net": net, "trades": trades}}

    moved: list[dict] = []
    monkeypatch.setattr(ledger, "check_trials", lambda *a: moved)
    moved[:] = [trial(120.0, 30), trial(0.0, 0)]  # the entry moved past the exit: it never trades
    gate = promotion._plateau("e", True)
    assert gate.passed and "1 made no trade" in gate.detail
    moved[:] = [trial(0.0, 0), trial(0.0, 0)]
    assert not promotion._plateau("e", True).passed
    moved[:] = [trial(120.0, 30), trial(-5.0, 12)]
    assert not promotion._plateau("e", True).passed


def test_the_holdout_waits_for_the_values_by_day_it_would_read(tmp_path, monkeypatch):
    from optra.data import daily_values
    from optra.research import promotion

    monkeypatch.setattr(daily_values, "daily_dir", lambda: tmp_path)
    kept = {f"2026-03-{d:02d}": 1.0 for d in range(2, 32)}
    daily_values.write_series(tmp_path / "SPY_gex.csv", kept)  # kept by day, stopped at the holdout
    daily_values.write_series(tmp_path / "VIX.csv", kept | {"2026-10-14": 20.0})
    daily_values.write_series(tmp_path / "FOMC.csv", {"2026-03-18": 1.0})  # an event list
    assert promotion.uncovered_series(date(2026, 4, 1), date(2026, 10, 15)) == ["SPY_gex (last 2026-03-31)"]
    daily_values.write_series(tmp_path / "SPY_gex.csv", kept | {"2026-10-09": 2.0})
    assert promotion.uncovered_series(date(2026, 4, 1), date(2026, 10, 15)) == []


def test_a_day_is_in_the_holdout_by_its_quotes_not_its_name_or_folder(days, monkeypatch, tmp_path):
    import shutil

    from optra import settings

    monkeypatch.setattr(settings, "config", lambda: {"holdout": {"SPY": "2025-03-06"}})
    with pytest.raises(ValueError, match="holdout"):
        ledger.resolve_days(days)
    renamed = tmp_path / "backtest" / "SPY" / "SPY-2025-02" / "20250228.parquet"  # the last day, named as a day before
    renamed.parent.mkdir()
    shutil.copy(days[-1], renamed)
    with pytest.raises(ValueError, match="holdout"):
        ledger.resolve_days([str(renamed)])
    elsewhere = tmp_path / "NOHOLD" / "NOHOLD-2025-03" / "20250307.parquet"  # a folder of no portfolio's
    elsewhere.parent.mkdir(parents=True)
    shutil.copy(days[-1], elsewhere)
    with pytest.raises(ValueError, match="holdout"):
        ledger.resolve_days([str(elsewhere)])
    first, reached = ledger.resolve_days(days[:3])
    assert [str(f) for f in first] == days[:3] and not reached
    assert ledger.resolve_days(days, reach_holdout=True)[1]  # a person's report may reach it, and it is recorded so


QUIET = """#include "strategy_registry.hpp"
#include "strategy.hpp"

namespace strategy {

class QuietProbeStrategy final : public Strategy {
  public:
    struct Setting {
        Param<int> wait{"wait", 5, 1, 60};
    };
    using Strategy::Strategy;
    void on_timer() override {}

  private:
    const Setting setting_ = read_setting<Setting>();
};

REGISTER_STRATEGY(QuietProbeStrategy);

} // namespace strategy
"""


def test_a_strategy_written_is_built_alone_and_backtests_load_it(own_strategy_dirs):
    out = strategies.put("quiet_probe", QUIET)
    assert out["built"] and out["refused"] == []
    assert out["classes"] == [{"name": "QuietProbeStrategy",
                               "parameters": [{"name": "timer_trigger", "default": 1.0, "min": 1.0, "max": 390.0,
                                               "integer": True},
                                              {"name": "wait", "default": 5.0, "min": 1.0, "max": 60.0,
                                               "integer": True}]}]
    assert not strategies.from_demo(strategies.strategy_listing()["QuietProbeStrategy"]["source"])
    broken = strategies.put("quiet_probe", QUIET.replace("on_timer() override {}", "on_timer() override"))
    assert not broken["built"] and any("error" in line for line in broken["errors"])
    with pytest.raises(ValueError, match="a demo strategy"):
        strategies.put("random_buy", QUIET)


def test_a_trial_that_stopped_before_its_last_day_is_reported_and_fails_review(days):
    from optra.research import backtest

    first = backtest.backtest(strategy="RandomBuyStrategy", setting=None, space={"seed": [4, 5]}, search={},
                              family="stops", symbol="SPY", start=None, end=None, costs={})
    stopped_id = first["trials"][0]["id"]
    why = "No day between 2024-06-20 and 2024-06-24, which the holding would skip: the run stops here"
    ledger.db().execute("UPDATE trials SET error = ? WHERE id = ?", (why, stopped_id))
    ledger.db().commit()
    out = backtest.backtest(strategy="RandomBuyStrategy", setting=None, space={"seed": [4, 5]}, search={},
                            family="stops", symbol="SPY", start=None, end=None, costs={})
    assert out["stopped"] == {stopped_id: why}
    gate = next(g for g in out["review"] if g["gate"] == "every trial ran all its days")
    assert not gate["met"] and gate["detail"].startswith("1 stopped early")


def test_a_strategy_reaches_an_account_only_when_installed_for_it(own_strategy_dirs, days):
    from optra.research import code_identity

    put = strategies.put("quiet_probe", QUIET)
    assert put["built"], put["errors"]
    research = strategies.library_dir() / "quiet_probe.so"
    assert not strategies.installed_library("paper", "quiet_probe.cpp").exists()
    files = code_identity.strategy_sources(research)
    library = strategies.install(files, "paper")
    assert library == strategies.installed_library("paper", "quiet_probe.cpp") and library.exists()
    assert not strategies.installed_library("live", "quiet_probe.cpp").exists()  # paper, not live
    assert code_identity.strategy_hash(library) == code_identity.strategy_hash(research)  # the same code, wherever it is
    installed = own_strategy_dirs["paper"] / "quiet_probe.cpp"
    strategies.uninstall(files, "paper")
    assert not library.exists() and not installed.exists()
    from optra.research import promotion

    assert versions.accounts_of("paper") == {"paper"} and versions.accounts_of("pilot") == {"paper", "live"}
    assert versions.accounts_of("review") == set()


def test_the_backtest_tool_answers_whole_and_counts_every_try(days):
    from optra.research import backtest

    out = backtest.backtest(strategy="RandomBuyStrategy", setting=None, space={"seed": [1, 2, 3]}, search={},
                            family="random buys", symbol="SPY", start=None, end=None, costs={})
    assert len(out["trials"]) == 3 and out["family_tries"] == 3 and out["days"] == len(days)
    assert out["best"]["id"] in {t["id"] for t in out["trials"]} and "expectancy_r" in out["control"]
    assert [g["gate"] for g in out["review"]] == ["deflated Sharpe", "overfitting probability",
                                                  "every trial ran all its days", "trades", "days with a trade",
                                                  "parameters moved", "costs doubled",
                                                  "fills of the size shown, a minute late"]
    assert out["stopped"] == {}
    # The control (RandomBuyStrategy, seed 1) is the first of these settings: the same trial, run once, counted once.
    assert ledger.families()["random buys"]["used"] == 3 and ledger.families()["control"]["used"] == 0
    again = backtest.backtest(strategy="RandomBuyStrategy", setting={"seed": 1}, space=None, search={},
                              family="random buys", symbol="SPY", start=None, end=None, costs={})
    assert len(again["trials"]) == 1 and ledger.families()["random buys"]["used"] == 3


def test_the_optimizer_climbs_a_known_hill_without_trying_a_point_twice():
    def value(x, y):
        return -((x - 3) ** 2) - (y - 7.5) ** 2

    tried, values = [], []
    for r in range(8):
        out = numerics.run_engine_task("optimize", {"lower": [0, 0], "upper": [10, 10], "integer": [True, False],
                                                    "tried": tried, "values": values, "batch": 4, "seed": r})
        assert out["status"] == "ok", out
        for x, y in out["result_summary"]["points"]:
            assert x == round(x) and 0 <= x <= 10 and 0 <= y <= 10
            tried.append([x, y])
            values.append(value(x, y))
    assert len({tuple(p) for p in tried}) == len(tried)  # nothing proposed twice
    best = tried[max(range(len(values)), key=values.__getitem__)]
    assert abs(best[0] - 3) <= 1 and abs(best[1] - 7.5) <= 1.5, best


def test_an_optimizers_rounds_are_counted_and_its_whole_run_is_where_a_version_is_found(days):
    from optra.research import optimizer, promotion

    out = optimizer.optimize(family="random buys", strategy="RandomBuyStrategy", days=days,
                             space={"hold_minutes": {"range": [15, 90]}}, costs=ledger.Costs(), rounds=2, batch=3)
    assert len(out["rounds"]) == 2 and out["settings"] == 6
    assert ledger.families()["random buys"]["used"] == 6  # each round counted once; the whole run reuses them
    whole = ledger.experiment(out["experiment_id"])
    assert len(whole["trials"]) == 6 and all(t["status"] == "done" for t in whole["trials"])
    assert all(ledger.experiment(r)["spec"]["check"] == optimizer.ROUND for r in out["rounds"])
    best = analysis.analysis(out["experiment_id"])["best"]
    version = versions.register_version("RandomBuyStrategy", best["setting"], "random buys",
                                      {"hypothesis": "-", "edge": "-", "fails_when": "-"})
    gates = {g.name: g for g in promotion._research(version)}
    assert out["experiment_id"] in gates["deflated Sharpe"].detail  # the whole run, not a round


def test_a_versions_underlying_is_its_own_trials_and_one_is_named_when_they_ran_on_several(monkeypatch):
    from optra.research import promotion

    def trial(root, check=None, parent=None):
        return {"id": f"{root}{check}", "days": [f"{root}/{root}-2024-01/20240102.parquet"], "holdout": False,
                "parent": parent, "check": check, "report": False, "costs": {}}

    found = [trial("SPXW"), trial("SPY", "other_underlying", "x")]  # a check on another underlying is not its own
    monkeypatch.setattr(versions, "version_trials", lambda _: found)
    assert versions.portfolio("v") == "SPXW"
    found.append(trial("SPY"))  # the same setting backtested on SPY too
    with pytest.raises(ValueError, match="name one underlying"):
        versions.portfolio("v")
    assert versions.portfolio("v", "SPY") == "SPY"
    assert promotion._found("v", "SPY")["id"] == "SPYNone"
    with pytest.raises(ValueError, match="name one underlying"):
        versions.portfolio("v", "QQQ")
    held = dict(trial("SPY"), holdout=True)  # its holdout report ran on SPY: what paper then runs on
    found.append(held)
    assert versions.portfolio("v") == "SPY"
    found[:] = [trial("SPXW"), dict(trial("SPY"), report=True)]  # a report shows it there, it was not found there
    assert versions.portfolio("v") == "SPXW"
