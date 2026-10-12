"""The live engine's day and making backtest days: the API the scheduler (systemd timers) drives; no page needs
them. Reports of strategies over backtest days are a tool (backtest_report)."""

from __future__ import annotations

from dataclasses import fields
from datetime import date
from pathlib import Path
from typing import Any, Dict, List, Optional

from fastapi import APIRouter, HTTPException
from pydantic import BaseModel, Field

from optra.services import recorded_days, synthetic_days
from optra.services import research, session
from optra.services.main import MainService
from optra.utils.files import repo_root

router = APIRouter()


def _bad_request(exc: Exception) -> HTTPException:
    return HTTPException(status_code=400, detail=str(exc))


def _under_repo(path: str) -> Path:
    p = Path(path)
    return p if p.is_absolute() else repo_root() / p


# ── the live engine's day ─────────────────────────────────────────────────────


@router.get("/api/session/check")
def api_session_check() -> Dict[str, Any]:
    """What is wrong with the live engine now (empty: nothing): not up, not connected to IB, no
    quotes, positions not agreeing with IB, halted, a strategy in error."""
    return {"status": "ok", "problems": session.problems(MainService._get_client_or_400())}


@router.post("/api/session/check")
def api_session_check_and_keep(notify: bool = False) -> Dict[str, Any]:
    """The same, kept as the last check: `message` is what to tell (empty: nothing). A problem
    is told once two checks in a row see it (an engine starting does not answer for a minute);
    back to normal names what was told. With notify it is sent ($OPTRA_NOTIFY)."""
    return session.check(MainService._get_client_or_400(), notify)


@router.post("/api/session/report")
def api_session_report(day: Optional[date] = None, record_dir: Optional[str] = None,
                       notify: bool = False) -> Dict[str, Any]:
    """The day's report (New York date, default today), written to data/reports/<day>.md and
    returned: fills and P&L by strategy, refusals and the halt, the positions check, and each
    strategy that traded against a backtest of the recorded day. With notify, a summary is sent."""
    try:
        return session.report(MainService._get_client_or_400(), day,
                              _under_repo(record_dir) if record_dir else None, notify)
    except ValueError as exc:
        raise _bad_request(exc) from exc


@router.get("/api/session/compare")
def api_session_compare(day: date, strategy: str, fee_rate: float = 0.65) -> Dict[str, Any]:
    """A paper day of one strategy against a backtest of the same recorded day, fill by fill."""
    try:
        return session.compare(MainService._get_client_or_400(), strategy, day, fee_rate)
    except ValueError as exc:
        raise _bad_request(exc) from exc


# ── backtest days ─────────────────────────────────────────────────────────────


class RecordedRequest(BaseModel):
    inputs: List[str] = Field(["data/recorded"], description="recorded CSVs or folders of them")
    out: str = Field("data/recorded-parquet", description="the data root written to")
    symbol: Optional[str] = Field(None, description="the portfolio's root (default: each CSV's folder)")
    overwrite: bool = False


@router.post("/api/data/recorded")
def api_data_recorded(req: RecordedRequest) -> Dict[str, Any]:
    """Quotes the live engine recorded (OPTRA_RECORD_DIR) as backtest days."""
    written, failed = recorded_days.convert([_under_repo(p) for p in req.inputs],
                                                  _under_repo(req.out), req.symbol, req.overwrite)
    return {"status": "ok" if not failed else "error", "written": [str(p) for p in written],
            "failed": failed}


class SyntheticRequest(BaseModel):
    start: date
    end: date
    symbol: str = "SPY"
    out: str = Field("data", description="the data root written to")
    overwrite: bool = False
    model: Dict[str, float] = Field(
        default_factory=dict,
        description="the model's fields to change: " + ", ".join(f.name for f in fields(synthetic_days.Model)))


@router.post("/api/data/synthetic")
def api_data_synthetic(req: SyntheticRequest) -> Dict[str, Any]:
    """Synthetic backtest days from a model whose answers are known: the underlying a random walk
    at realized_vol, the options priced at implied_vol (with skew and smile)."""
    known = {f.name: f.type for f in fields(synthetic_days.Model)}
    unknown = sorted(set(req.model) - set(known))
    if unknown:
        raise HTTPException(status_code=400, detail=f"unknown model fields: {', '.join(unknown)}")
    model = synthetic_days.Model(**{k: (int(v) if known[k] == "int" else v) for k, v in req.model.items()})
    try:
        synthetic_days.check(model)
        written = synthetic_days.generate(model, req.start, req.end, _under_repo(req.out),
                                                req.symbol, req.overwrite)
    except ValueError as exc:  # ConversionError too: a day already there without overwrite
        raise _bad_request(exc) from exc
    return {"status": "ok", "written": [str(p) for p in written]}
