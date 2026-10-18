"""Strategy versions: a class with its setting frozen and the code it is, under a family, with its card; the stage it
is at and every move between stages (research.promotion decides them), a move under way (its intent: written first,
then carried out step by step, each step safe to repeat), and the strategy libraries installed for an account's
engine. The ledger (research.ledger) holds the trials a version's evidence is read from; research.evidence what the
system recorded of it since (paper days, holdout uses)."""

from __future__ import annotations

import json
from typing import Any

from optra.research.ledger import DEFAULT_BUDGET, code_of, digest
from optra.research.space import canonical, resolved
from optra.research.store import db, lock as _lock, now


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
    experiment's days, whether that experiment could reach the holdout (the promotion report's) and the experiment it
    was a robustness check of (parent) and which check (analysis.robustness), if it was one."""
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
             "check": json.loads(r["spec"]).get("check")} for r in rows]


def record_install(file: str, strategy_hash: str, account: str, installed: bool, note: str) -> None:
    """A strategy installed for an account's engine (strategy/paper, strategy/live), or taken off it, and on whose
    word."""
    with _lock:
        db().execute("INSERT INTO strategy_installs VALUES (?, ?, ?, ?, ?, ?)",
                     (now(), file, account, strategy_hash, int(installed), note))


def set_pending(version_id: str, to: str, note: str) -> None:
    """The move to `to` written before any step of it runs (promotion.advance carries it out)."""
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
