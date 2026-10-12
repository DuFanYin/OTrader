"""The run history as tools: look a run up (and run it again), compare two runs, list the recent ones. Every result's
run_id finds it; listing is off on a public server (services/run_store.py)."""

from optra.schemas.tools import CompareRunsRequest, GetRunRequest, ListRunsRequest
from optra.infra import run_store
from optra.tools.base import Result, Tool, ToolError


def _load(run_id: str) -> dict:
    run = run_store.store().get(run_id)
    if run is None:
        raise ToolError(f"no run {run_id}: runs are kept for a while (lookups such as option_chain are not kept)", 404)
    return run


def _numbers(value, prefix="") -> dict:
    """The scalar leaves of nested dicts, by dotted key (lists are left out)."""
    out = {}
    for k, v in (value or {}).items():
        key = f"{prefix}{k}"
        if isinstance(v, dict):
            out.update(_numbers(v, key + "."))
        elif isinstance(v, (int, float, str, bool)) or v is None:
            out[key] = v
    return out


def differences(a: dict, b: dict) -> list[dict]:
    """What differs between two dicts' scalar leaves: each key with both values and, for numbers, the change."""
    fa, fb = _numbers(a), _numbers(b)
    rows = []
    for key in sorted(fa.keys() | fb.keys()):
        va, vb = fa.get(key), fb.get(key)
        if va == vb:
            continue
        row = {"key": key, "a": va, "b": vb}
        if all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in (va, vb)):
            row["change"] = vb - va
            row["relative_change"] = (vb - va) / abs(va) if va else None
        rows.append(row)
    return rows


async def get_run(req: GetRunRequest) -> Result:
    run = _load(req.run_id)
    summary = {k: run[k] for k in ("run_id", "tool", "created_at", "input_params", "result_summary", "warnings")}
    if req.rerun:
        from optra.tools import call
        again = await call(run["tool"], run["input_params"])
        summary["rerun"] = {"run_id": again["run_id"], "result_summary": again["result_summary"],
                            "differences": differences(run["result_summary"], again["result_summary"])}
    return Result(summary=summary, details=run["result_details"])


def compare_runs(req: CompareRunsRequest) -> Result:
    a, b = _load(req.run_a), _load(req.run_b)
    warnings = [] if a["tool"] == b["tool"] else [f"the runs are of different tools: {a['tool']} and {b['tool']}"]
    return Result(summary={"tools": [a["tool"], b["tool"]], "created_at": [a["created_at"], b["created_at"]],
                           "inputs": differences(a["input_params"], b["input_params"]),
                           "results": differences(a["result_summary"], b["result_summary"])},
                  warnings=warnings)


def list_runs(req: ListRunsRequest) -> Result:
    return Result(summary={"runs": run_store.store().list(req.tool, req.limit)})


HISTORY = "Every result has a run_id; the lab keeps the runs of its computations for a while. "
RUN = "3f2c9a5e-0b1d-4c8e-9f7a-2d6b1e8c4a90"  # the examples' stand-in for a real run_id

TOOLS = [
    Tool("list_runs", "List recent runs",
         HISTORY + "Lists the latest runs, newest first, optionally of one tool: per run its id, tool, time, a few inputs "
         "and headline numbers.",
         ListRunsRequest, list_runs, "/runs", method="GET", engine=False, record=False, example={"limit": 5}),
    Tool("compare_runs", "Compare two runs",
         HISTORY + "Compares two runs: every input and every number of result_summary that differs, with the change and "
         "the relative change. Use it to see what moved a result.",
         CompareRunsRequest, compare_runs, "/runs/compare", method="GET", engine=False, record=False,
         example={"run_a": RUN, "run_b": RUN}),
    Tool("get_run", "Look up a run",
         HISTORY + "Returns a run by its run_id: its tool, time, inputs, result_summary, warnings, and result_details. "
         "With rerun, runs the same tool again on the same inputs and lists what changed (live market data may have "
         "moved; simulations are seeded, so they repeat).",
         GetRunRequest, get_run, "/runs/{run_id}", method="GET", engine=False, record=False,
         example={"run_id": RUN}),
]
