"""The agent evals hold together offline: every task's truth comes from the tools, the checks accept a right answer and
reject a wrong one, and the runner drives the tool-use loop (against a scripted model; the real run needs an API key)."""

from __future__ import annotations

import asyncio
import json

import httpx  # noqa: E402

from evals import run  # noqa: E402
from evals.tasks import TASKS, contains_all, near, numbers  # noqa: E402


def test_truths_and_checks():
    for task in (t for t in TASKS if not t.live):
        truth = task.truth()
        if isinstance(truth, list):
            right, wrong = "Change " + " to ".join(truth), "Change n_paths to 1"
        else:
            right, wrong = f"The answer is {truth:,.4f}.", f"The answer is {truth * 1.5 + 1:.4f}."
        assert task.check(right, truth), (task.id, right)
        assert not task.check(wrong, truth), (task.id, wrong)


def test_numbers_and_checks():
    assert numbers("about 1,234.5 and -0.25 or 3e-2") == [1234.5, -0.25, 0.03]
    assert near(0.01)("roughly 4.178", 4.1783) and not near(0.01)("roughly 4.3", 4.1783)
    assert contains_all("Raise ito_n to 4,000.", ["ito_n", "4000"])


def test_runner_drives_the_tool_loop():
    task = next(t for t in TASKS if t.id == "bs_price")
    calls = []

    def model(request: httpx.Request) -> httpx.Response:
        body = json.loads(request.content)
        calls.append(body)
        assert {t["name"] for t in body["tools"]} >= {"price_option", "screen_strategies"}
        if len(body["messages"]) == 1:
            return httpx.Response(200, json={"stop_reason": "tool_use", "content": [
                {"type": "tool_use", "id": "t1", "name": "price_option",
                 "input": {"spot": 100, "strike": 105, "rate": 0.03, "vol": 0.2, "maturity": 0.5}}]})
        result = json.loads(body["messages"][-1]["content"][0]["content"])
        return httpx.Response(200, json={"stop_reason": "end_turn", "content": [
            {"type": "text", "text": f"The Black-Scholes price is {result['result_summary']['bs']:.4f}."}]})

    async def go():
        async with httpx.AsyncClient(transport=httpx.MockTransport(model)) as client:
            return await run.evaluate([task], client, "test-model")
    [row] = asyncio.run(go())
    assert row["passed"] and row["tools"] == ["price_option"] and len(calls) == 2
