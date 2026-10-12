"""The tool registry's parts: what a tool is, what it returns, and the one envelope every result comes in.

A tool is declared once (Tool) and every interface is made from the declarations: the HTTP routes (optra.api.tools)
and the Python calls (optra.tools.call, call_sync), used by the tests. A tool's `run` takes
its validated input model and returns a Result; `call` adds the envelope around it.
"""

from __future__ import annotations

import asyncio
import inspect
import math
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timezone
from time import perf_counter
from typing import Any, Awaitable, Callable, Literal

from pydantic import BaseModel

from optra.infra import numerics, run_store
from optra.version import CONTRACT_VERSION

SIGNIFICANT_DIGITS = 12  # floats in a response keep this many significant digits (drops 0.30000000000000004 noise)


class ToolError(Exception):
    """A request the tool cannot answer, with the HTTP status that fits and what to change."""

    def __init__(self, message: str, status: int = 400):
        super().__init__(message)
        self.message, self.status = message, status


@dataclass
class Result:
    """What a tool computed. notes say how (data sources, methods); warnings say what may make it less trustworthy."""
    summary: dict
    details: dict = field(default_factory=dict)
    notes: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    diagnostics: dict = field(default_factory=dict)


@dataclass(frozen=True)
class Tool:
    name: str            # snake_case, what agents call it
    title: str
    description: str     # for a model: what it computes, when to use it, what comes back
    input: type[BaseModel]
    run: Callable[[Any], Result | Awaitable[Result]]
    path: str            # its HTTP route under /api
    method: Literal["GET", "POST"] = "POST"
    cost: Literal["instant", "seconds", "heavy"] = "instant"  # at the defaults
    engine: bool = True      # needs the C++ engine
    network: bool = False    # fetches market data (falls back to cached or fixed data offline)
    live: bool = False       # asks the running live engine (gRPC): fails without one
    record: bool = True      # kept in the run history (data lookups and the history's own tools are not)
    example: dict = field(default_factory=dict)  # arguments that run it

    def schema(self) -> dict:
        """The input's JSON Schema, with the example."""
        return {**self.input.model_json_schema(), "examples": [self.example]}


def norm(value):
    """A response value made JSON-safe: non-finite floats become null, floats keep SIGNIFICANT_DIGITS digits."""
    if isinstance(value, bool) or value is None or isinstance(value, (int, str)):
        return value
    if isinstance(value, float):
        return float(f"{value:.{SIGNIFICANT_DIGITS}g}") if math.isfinite(value) else None
    if isinstance(value, (list, tuple)):
        return [norm(v) for v in value]
    if isinstance(value, dict):
        return {k: norm(v) for k, v in value.items()}
    return value


def envelope(tool: Tool, params: BaseModel, result: Result, ms: float) -> dict:
    """The one shape every tool's result comes in."""
    return _envelope(tool.name, params.model_dump(), result, ms)


def _envelope(tool: str, inputs: dict, result: Result, ms: float) -> dict:
    return {
        "contract_version": CONTRACT_VERSION,
        "run_id": str(uuid.uuid4()),
        "tool": tool,
        "status": "ok",
        "input_params": norm(inputs),
        "result_summary": norm(result.summary),
        "result_details": norm(result.details),
        "warnings": result.warnings,
        "diagnostics": norm({
            "compute_ms": ms,
            "engine_available": numerics.is_engine_available(),
            "threads": numerics.get_max_threads(),
            "notes": result.notes,
            **result.diagnostics,
        }),
        "created_at": datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
    }


async def run_tool(tool: Tool, params: BaseModel) -> dict:
    """Runs a tool on its validated input and returns the envelope, kept in the run history (services/run_store.py).
    A synchronous tool runs in a worker thread, so a long computation never holds up the event loop."""
    if tool.engine and not numerics.is_engine_available():
        raise ToolError(numerics.ENGINE_PROBLEM, 503)
    t0 = perf_counter()
    if inspect.iscoroutinefunction(tool.run):
        result = await tool.run(params)
    else:
        result = await asyncio.to_thread(tool.run, params)
    out = envelope(tool, params, result, (perf_counter() - t0) * 1000.0)
    if tool.record:
        await asyncio.to_thread(run_store.save_quietly, out)
    return out


def engine_task(task_type: str, payload: dict, trace_prefix: str | None = None) -> Result:
    """One engine task as a Result; an engine error becomes a ToolError."""
    request = {**payload, "contract_version": CONTRACT_VERSION}
    request.setdefault("trace_id", f"{trace_prefix or task_type}-{uuid.uuid4().hex[:12]}")
    r = numerics.run_engine_task(task_type, request)
    if r.get("status") != "ok":
        err = r.get("error") or {}
        raise ToolError(f"{task_type} failed: {err.get('message') or err.get('code') or 'engine error'}",
                        503 if err.get("code") == "engine_unavailable" else 400)
    diag = {k: v for k, v in (r.get("diagnostics") or {}).items() if k in ("runtime_ms", "compute_path")}
    return Result(summary=r.get("result_summary", {}), details=r.get("result_details", {}), diagnostics=diag)
