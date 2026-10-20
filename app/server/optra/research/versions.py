"""Strategy versions: a class with its setting frozen and the code it is, under a family, with its card; the stage it
is at and every move between stages (research.promotion decides them), a move under way (its intent: written first,
then carried out step by step, each step safe to repeat), and the strategy libraries installed for an account's
engine; and where a version trades, carried out (advance): its code installed for the accounts its stage trades on,
its strategy started on or taken off the paper engine. The ledger (research.ledger) holds the trials a version's
evidence is read from; research.evidence what the system recorded of it since (paper days)."""

from __future__ import annotations

import json
from typing import Any

from optra.research import code_identity, ledger, strategies
from optra.research.ledger import DEFAULT_BUDGET, code_of, digest
from optra.research.space import canonical, resolved
from optra.research.store import db, lock as _lock, now

LIVE_STRATEGIES = "/etc/optra/live-strategies"  # root's: the live account's engine runs only what it lists


def register_version(strategy: str, setting: dict[str, float], family: str, card: dict[str, str]) -> str:
    """A strategy version: the class, its setting frozen and the code it is now (code_of), under a family, with its
    card. The same class, setting and code are one version: registering it again returns it as it is."""
    code = code_of(strategy)
    setting = resolved(strategy, setting)
    version_id = digest({"strategy": strategy, "setting": setting, "code": code})
    with _lock:
        con = db()
        con.execute("BEGIN IMMEDIATE")
        con.execute("INSERT OR IGNORE INTO families VALUES (?, ?)", (family, DEFAULT_BUDGET))
        con.execute("INSERT OR IGNORE INTO strategy_versions (id, strategy, setting, code, family, card, stage, created_at) "
                    "VALUES (?, ?, ?, ?, ?, ?, 'research', ?)",
                    (version_id, strategy, canonical(setting), canonical(code), family, canonical(card), now()))
        con.execute("COMMIT")
    return version_id


def strategy_version(version_id: str) -> dict[str, Any]:
    row = db().execute("SELECT * FROM strategy_versions WHERE id = ?", (version_id,)).fetchone()
    if row is None:
        raise KeyError(version_id)
    events = db().execute("SELECT at, from_stage, to_stage, note FROM stage_events WHERE version_id = ? ORDER BY at",
                          (version_id,)).fetchall()
    return {**dict(row), "setting": json.loads(row["setting"]), "code": json.loads(row["code"]),
            "card": json.loads(row["card"]), "events": [dict(e) for e in events]}


def strategy_versions(stage: str | None = None) -> list[dict[str, Any]]:
    rows = db().execute("SELECT id, strategy, setting, family, stage, pending, created_at FROM strategy_versions "
                        "WHERE ? IS NULL OR stage = ? ORDER BY created_at DESC", (stage, stage)).fetchall()
    return [{**dict(r), "setting": json.loads(r["setting"])} for r in rows]


def set_stage(version_id: str, stage: str, note: str) -> None:
    with _lock:
        con = db()
        con.execute("BEGIN IMMEDIATE")
        row = con.execute("SELECT stage FROM strategy_versions WHERE id = ?", (version_id,)).fetchone()
        if row is None:
            con.execute("ROLLBACK")
            raise KeyError(version_id)
        con.execute("UPDATE strategy_versions SET stage = ?, pending = NULL, pending_note = NULL, pending_error = NULL "
                    "WHERE id = ?", (stage, version_id))
        con.execute("INSERT INTO stage_events VALUES (?, ?, ?, ?, ?)", (version_id, now(), row["stage"], stage, note))
        con.execute("COMMIT")


def holdout_reports(family: str) -> int:
    """The versions of a family that have had their holdout report (a finished trial in an experiment that reached the
    holdout)."""
    return db().execute("""
        SELECT COUNT(DISTINCT v.id) FROM strategy_versions v
        JOIN trials t ON t.strategy = v.strategy AND t.setting = v.setting AND t.code = v.code
        JOIN experiment_trials et ON et.trial_id = t.id JOIN experiments e ON e.id = et.experiment_id
        WHERE v.family = ? AND e.holdout = 1 AND t.status = 'done'""", (family,)).fetchone()[0]


def version_trials(version_id: str) -> list[dict[str, Any]]:
    """The finished trials of a version (its class, setting and code, whatever the costs), each with its costs, its error
    (a run that stopped before its last day), its
    experiment's days, whether they reach the holdout, whether the experiment was a report (it shows strategies, it
    finds nothing), and the experiment it was a robustness check of (parent) and which check (analysis.robustness), if
    it was one."""
    v = strategy_version(version_id)
    rows = db().execute("""
        SELECT t.id, t.summary, t.costs, t.error, e.id AS experiment_id, e.days, e.holdout, e.parent, e.spec
        FROM trials t
        JOIN experiment_trials et ON et.trial_id = t.id JOIN experiments e ON e.id = et.experiment_id
        WHERE t.strategy = ? AND t.setting = ? AND t.code = ? AND t.status = 'done'""",
                        (v["strategy"], canonical(v["setting"]), canonical(v["code"]))).fetchall()
    return [{"id": r["id"], "experiment_id": r["experiment_id"], "summary": json.loads(r["summary"]),
             "costs": json.loads(r["costs"]), "error": r["error"],
             "days": json.loads(r["days"]), "holdout": bool(r["holdout"]), "parent": r["parent"],
             "check": (spec := json.loads(r["spec"])).get("check"), "report": bool(spec.get("report"))}
            for r in rows]


def record_install(file: str, strategy_hash: str, account: str, installed: bool, note: str) -> None:
    """A strategy installed for an account's engine (strategy/paper, strategy/live), or taken off it, and on whose
    word."""
    with _lock:
        db().execute("INSERT INTO strategy_installs VALUES (?, ?, ?, ?, ?, ?)",
                     (now(), file, account, strategy_hash, int(installed), note))


def set_pending(version_id: str, to: str, note: str) -> None:
    """The move to `to` written before any step of it runs (advance carries it out)."""
    with _lock:
        if db().execute("UPDATE strategy_versions SET pending = ?, pending_note = ?, pending_error = NULL WHERE id = ?",
                        (to, note, version_id)).rowcount == 0:
            raise KeyError(version_id)


def set_pending_error(version_id: str, error: str) -> None:
    """Why the last step of the move under way failed: it stays under way, to be carried out again."""
    with _lock:
        db().execute("UPDATE strategy_versions SET pending_error = ? WHERE id = ?", (error[:2000], version_id))


def pending() -> list[str]:
    """The versions with a move under way."""
    return [r[0] for r in db().execute("SELECT id FROM strategy_versions WHERE pending IS NOT NULL ORDER BY created_at")]


# ---------------------------------------------------------------- where a version trades

def own_trials(version_id: str) -> list[dict[str, Any]]:
    """The version's own trials in research: not checks of another (one of which runs on another underlying), not
    reports, not on the holdout."""
    return [t for t in version_trials(version_id)
            if not t["holdout"] and not t["parent"] and not t["check"] and not t["report"] and t["days"]]


def root_of(trial: dict[str, Any]) -> str:
    return trial["days"][0].split("/")[0]


def portfolio(version_id: str, root: str | None = None) -> str:
    """The underlying the version was tested on (its own trials' days); when it was tested on several, root if it is
    one of them, else the one its holdout report ran on."""
    roots = {root_of(t) for t in own_trials(version_id)}
    if root is not None and root in roots:
        return root
    if len(roots) > 1 and root is None:
        held = {root_of(t) for t in version_trials(version_id) if t["holdout"] and t["days"]}
        roots = held if len(held) == 1 else roots
    if len(roots) != 1:
        raise ValueError(f"the version was tested on {sorted(roots) or 'nothing'}: name one underlying to run it on")
    return roots.pop()


def _sources(v: dict[str, Any]) -> dict[str, str] | None:
    """The version's files to install, or None for a demo strategy (every engine loads those)."""
    files = ledger.strategy_source(v["code"]["strategy"])
    cpp = next(name for name in files if name.endswith(".cpp"))
    return None if (strategies.demo_dir() / cpp).exists() else files


def accounts_of(stage: str) -> set[str]:
    """The accounts a version at this stage trades on: paper from paper on, and live from pilot on."""
    return {"paper"} if stage == "paper" else {"paper", "live"} if stage in ("pilot", "live") else set()


def _place(version_id: str, v: dict[str, Any], to: str, note: str) -> None:
    """Installs the version for the accounts `to` trades on and takes it off the others, each recorded. Refused
    (ValueError) when it does not build, or the code installed is not the version's."""
    sources = _sources(v)
    if sources is None:
        return
    cpp = next(n for n in sources if n.endswith(".cpp"))
    for account in strategies.ACCOUNTS:
        installed = (strategies.installed_dir(account) / cpp).exists()
        wanted = account in accounts_of(to)
        if wanted and not installed:
            strategies.install(sources, account)
            if code_identity.strategy_hash(strategies.installed_library(account, cpp)) != v["code"]["strategy"]:
                strategies.uninstall(sources, account)
                raise ValueError("the installed code is not the version's (a header of strategy/demo it includes "
                                 "changed since): register it again")
            record_install(cpp, v["code"]["strategy"], account, True, f"{version_id} to {to}: {note}")
        elif installed and not wanted and stage_installed(v, account, cpp):
            strategies.uninstall(sources, account)
            record_install(cpp, v["code"]["strategy"], account, False, f"{version_id} to {to}: {note}")


def live_approval(v: dict[str, Any]) -> str:
    """The version's line in the live account's list of approved strategies, which its engine reads at start and runs
    nothing else (IbSettings::live_strategies): the class, the code and the setting."""
    setting = (f"{k}={float(x)!r}" for k, x in sorted(v["setting"].items()))
    return " ".join([v["strategy"], v["code"]["strategy"], *setting])


def engine_name(version_id: str, v: dict[str, Any]) -> str:
    """The version's strategy on an engine."""
    return f"{v['strategy']}_{version_id}"


def paper_engine():
    from optra.infra import live
    try:
        return live.client("paper")
    except Exception as exc:  # the server runs without the engine
        raise ValueError(f"the paper engine does not answer: {getattr(exc, 'detail', exc)}") from exc


def _start_on_paper(version_id: str, v: dict[str, Any]) -> str:
    """The version as a strategy of the paper engine, added (or restored, if it ran before) and started. Its name."""
    client = paper_engine()
    name = engine_name(version_id, v)
    refused = client.get_strategy_classes()["refused"]  # loads the libraries installed since
    there = {s["strategy_name"]: s for s in client.list_strategies()}
    if name not in there:
        if name in client.get_removed_strategies():
            client.restore_strategy(name)
        else:
            try:
                client.add_strategy(v["strategy"], portfolio(version_id), v["setting"], name)
            except Exception as exc:
                raise ValueError(f"the paper engine does not take {v['strategy']}: {getattr(exc, 'details', exc)}"
                                 + (f" (libraries it refused: {'; '.join(refused)})" if refused else "")) from exc
        there[name] = {"status": "added"}
    if there[name]["status"] == "added":
        client.init_strategy(name)
    if there[name]["status"] != "running":
        client.start_strategy(name)
    return name


def _stop_on_paper(version_id: str, v: dict[str, Any]) -> str:
    """The version's strategy on the paper engine stopped and taken off, if it is there; what was done."""
    client = paper_engine()
    name = engine_name(version_id, v)
    if name not in {s["strategy_name"] for s in client.list_strategies()}:
        return f"{name} is not on the paper engine"
    if next(s for s in client.list_strategies() if s["strategy_name"] == name)["status"] == "running":
        client.stop_strategy(name)
    if client.remove_strategy(name)["removed"]:
        return f"{name} stopped and taken off the paper engine"
    return f"{name} stopped; it still holds positions, so it stays on the paper engine until they are closed"


def stage_installed(v: dict[str, Any], account: str, cpp: str) -> bool:
    """The library built for the account is this version's code (not another's, nor one a person installed)."""
    library = strategies.installed_library(account, cpp)
    return library.exists() and code_identity.strategy_hash(library) == v["code"]["strategy"]


def advance(version_id: str) -> None:
    """Carries out the move under way, each step safe to run again: off the paper engine when it no longer trades
    there, its code installed for the accounts it trades on and taken off the others (_place), on the paper engine
    when it starts trading there; then the stage. A step that fails leaves the move under way with why (ValueError):
    advance_pending carries it out again."""
    v = strategy_version(version_id)
    to, note = v["pending"], v["pending_note"] or ""
    if to is None:
        return
    stage = v["stage"]
    try:
        if "paper" in accounts_of(stage) and "paper" not in accounts_of(to):
            note += f" ({_stop_on_paper(version_id, v)})"
        _place(version_id, v, to, note)
        if "paper" in accounts_of(to) and "paper" not in accounts_of(stage):
            note += f" (runs on the paper engine as {_start_on_paper(version_id, v)})"
        if "live" in accounts_of(to) and "live" not in accounts_of(stage):
            note += f" (the live account's engine runs it once root adds to {LIVE_STRATEGIES}: {live_approval(v)})"
    except Exception as exc:
        why = str(getattr(exc, "detail", exc))
        set_pending_error(version_id, why)
        raise ValueError(f"the move to {to} is under way and stopped: {why}; it is tried again at the next check "
                         "(or promote again)") from exc
    set_stage(version_id, to, note)


def advance_pending() -> list[str]:
    """Every move under way carried out again (the session check's); what still stops them."""
    stuck = []
    for version_id in pending():
        try:
            advance(version_id)
        except ValueError as exc:
            stuck.append(f"version {version_id}: {exc}")
    return stuck


def version_of(strategy_name: str) -> str | None:
    """The version a strategy on the paper engine runs (engine_name), or None for one a person added."""
    _, _, version_id = strategy_name.rpartition("_")
    try:
        v = strategy_version(version_id)
    except KeyError:
        return None
    return version_id if engine_name(version_id, v) == strategy_name else None
