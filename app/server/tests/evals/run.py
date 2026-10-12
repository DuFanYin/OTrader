"""Runs the agent tasks (evals/tasks.py) with Claude using the lab's tools, and checks the answers.

    ANTHROPIC_API_KEY=... uv run python tests/evals/run.py [--model claude-sonnet-5-5] [--task bs_price] [--live]

The model gets every tool of the registry with its description and input schema (as listed at /api/tools), the tools run in-process,
and the answer is checked against the truth the tools compute. When a task fails, the tool's description or error message
is what to improve. Live tasks (the Deribit chain) run only with --live.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
from pathlib import Path

# Run as a script (from app/server: uv run python tests/evals/run.py): optra and evals import by name
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import httpx  # noqa: E402
from pydantic import ValidationError  # noqa: E402

from evals.tasks import TASKS, Task  # noqa: E402
from optra.tools import TOOLS, ToolError, call  # noqa: E402


LIST_LIMIT = 40  # longer lists in a tool result are cut, to keep it within the model's context

INSTRUCTIONS = """Optra prices, calibrates, stresses, hedges and screens options with its C++ engine, and backtests
strategies on the trading engine. No tool places an order.

Units: rates, vols, yields and drifts are decimals per year (0.05 = 5%); maturities are in years; prices are in the units of
spot (USD for BTC and ETH). Every result has result_summary (the headline numbers), result_details (tables and curves),
warnings (read them: they say when a number is less trustworthy, e.g. a stale chain or a failed check) and diagnostics.

Typical flows:
- a BTC option: market_snapshot for spot, vol and rate, then price_option;
- a trade idea: screen_strategies, then price_strategy or stress_test on a strategy's legs;
- a doubtful number: validate with the relevant checks, or benchmark_methods / lattice_convergence to see the numerical error;
- a strategy: backtest_days for the days there are, screen_strategies on one of them, backtest_combo on a strategy it
  finds, hedge_study for its hedge; backtest_report compares the engine's strategy classes (strategy_classes).
"""


def compact(value, limit: int = LIST_LIMIT):
    """The value with every list longer than `limit` cut to its first `limit` items and a note of how many were left out."""
    if isinstance(value, dict):
        return {k: compact(v, limit) for k, v in value.items()}
    if isinstance(value, list):
        head = [compact(v, limit) for v in value[:limit]]
        return head + [f"… {len(value) - limit} more not shown"] if len(value) > limit else head
    return value


API = "https://api.anthropic.com/v1/messages"
MODEL = "claude-sonnet-5-5"
MAX_TURNS = 12
SYSTEM = INSTRUCTIONS + "\nUse the tools for every number. Finish with a short answer that states the numbers asked for."


def tool_definitions() -> list[dict]:
    return [{"name": t.name, "description": t.description, "input_schema": t.input.model_json_schema()} for t in TOOLS.values()]


async def run_tool(name: str, arguments: dict) -> tuple[str, bool]:
    try:
        return json.dumps(compact(await call(name, arguments)), separators=(",", ":")), False
    except ToolError as exc:
        return exc.message, True
    except ValidationError as exc:
        return f"invalid arguments: {exc}", True


async def ask(task: Task, client: httpx.AsyncClient, model: str) -> tuple[str, list[str]]:
    """The model's final answer to a task, and the tools it called."""
    messages, called = [{"role": "user", "content": task.prompt}], []
    for _ in range(MAX_TURNS):
        r = await client.post(API, json={"model": model, "max_tokens": 2048, "system": SYSTEM, "tools": tool_definitions(),
                                         "messages": messages})
        r.raise_for_status()
        reply = r.json()
        messages.append({"role": "assistant", "content": reply["content"]})
        uses = [b for b in reply["content"] if b["type"] == "tool_use"]
        if reply.get("stop_reason") != "tool_use" or not uses:
            return "".join(b.get("text", "") for b in reply["content"] if b["type"] == "text"), called
        results = []
        for use in uses:
            called.append(use["name"])
            content, is_error = await run_tool(use["name"], use["input"])
            results.append({"type": "tool_result", "tool_use_id": use["id"], "content": content, "is_error": is_error})
        messages.append({"role": "user", "content": results})
    return "", called


async def evaluate(tasks: list[Task], client: httpx.AsyncClient, model: str) -> list[dict]:
    rows = []
    for task in tasks:
        answer, called = await ask(task, client, model)
        truth = await asyncio.to_thread(task.truth)
        rows.append({"task": task.id, "passed": task.check(answer, truth), "truth": truth, "tools": called, "answer": answer})
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--model", default=MODEL)
    parser.add_argument("--task", action="append", help="run only this task (repeatable)")
    parser.add_argument("--live", action="store_true", help="include the tasks that read the live chain")
    args = parser.parse_args()
    key = os.environ.get("ANTHROPIC_API_KEY")
    if not key:
        print("set ANTHROPIC_API_KEY to run the evals", file=sys.stderr)
        return 2
    tasks = [t for t in TASKS if (not args.task or t.id in args.task) and (args.live or not t.live)]

    async def run():
        headers = {"x-api-key": key, "anthropic-version": "2023-06-01"}
        async with httpx.AsyncClient(headers=headers, timeout=300) as client:
            return await evaluate(tasks, client, args.model)
    rows = asyncio.run(run())
    for r in rows:
        print(f"{'PASS' if r['passed'] else 'FAIL'}  {r['task']:18s} tools: {', '.join(r['tools']) or '–'}")
        if not r["passed"]:
            print(f"      truth {r['truth']!r}; answer: {r['answer'][:300]!r}")
    print(f"\n{sum(r['passed'] for r in rows)}/{len(rows)} passed with {args.model}")
    return 0 if all(r["passed"] for r in rows) else 1


if __name__ == "__main__":
    sys.exit(main())
