"""The 5-minute check tells a problem once two checks in a row see it, and back to normal naming it."""

from __future__ import annotations

import pytest

from optra.services import session


@pytest.fixture()
def run(tmp_path, monkeypatch):
    sent: list[str] = []
    seen: list[list[str]] = []
    monkeypatch.setattr(session, "_data", lambda: tmp_path)
    monkeypatch.setattr(session, "notify", sent.append)
    monkeypatch.setattr(session, "problems", lambda client: seen.pop(0))
    monkeypatch.setattr(session.versions, "advance_pending", lambda: [])  # no ledger here

    def check(*problems: str) -> list[str]:
        seen.append(list(problems))
        before = len(sent)
        session.check(None, True)
        return sent[before:]
    return check


def test_a_restart_is_not_told(run):
    assert run("the engine does not answer (UNAVAILABLE)") == []
    assert run() == []


def test_a_lasting_problem_is_told_once_and_its_end_names_it(run):
    down = "the engine does not answer (UNAVAILABLE)"
    assert run(down) == []
    assert run(down) == [f"Optra: {down}"]
    assert run(down) == []
    assert run() == [f"Optra: back to normal; was: {down}"]
    assert run() == []


def test_the_earlier_state_file_still_reads(run, tmp_path):
    (tmp_path / "session-check.json").write_text('["halted"]')
    assert run() == ["Optra: back to normal; was: halted"]
