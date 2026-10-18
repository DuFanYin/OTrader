"""Backtest-related HTTP API: files, run/cancel."""

from __future__ import annotations

from typing import Any, Dict, List

from fastapi import APIRouter, Body, Depends

from optra.infra import access
from optra.utils.files import list_files
from optra.services import backtest


router = APIRouter()
ANYONE = [Depends(access.anyone)]
PERSON = [Depends(access.person_only)]


@router.get("/api/files", dependencies=ANYONE)
def api_files() -> List[Dict[str, Any]]:
    """List .dbn and .parquet under download/ and data/."""
    return [f.__dict__ for f in list_files()]


@router.post("/api/run_backtest", dependencies=PERSON)
def api_run_backtest(
    request: Dict[str, Any] = Body(...),
):
    """Run backtest (blocking); return results + metrics."""
    return backtest.run_backtest(request)


@router.post("/api/backtest/cancel", dependencies=PERSON)
def api_backtest_cancel() -> Dict[str, Any]:
    """Cancel running C++ backtest (if any)."""
    return backtest.cancel_backtest()

