"""The tools: one registry behind the HTTP API (a route per tool, /api/tools) and the in-process Python calls, each
tool documented well enough to be called from its schema alone, and one envelope for every result. Offline: market
data is faked to fail."""

from __future__ import annotations

import asyncio
import json
import os
import tempfile


import httpx  # noqa: E402
import pytest  # noqa: E402
from fastapi.testclient import TestClient  # noqa: E402
from pydantic import BaseModel, ValidationError  # noqa: E402

from main import create_app  # noqa: E402
from optra.infra import numerics  # noqa: E402
from optra.services import market_data  # noqa: E402
from optra.tools import TOOLS, ToolError, call, call_sync  # noqa: E402
from optra.tools.runs import RUN  # noqa: E402
from optra.version import CONTRACT_VERSION  # noqa: E402

APP = create_app(engine=False)  # the server without the live engine

ENVELOPE = {"contract_version", "run_id", "tool", "status", "input_params", "result_summary", "result_details",
            "warnings", "diagnostics", "created_at"}


@pytest.fixture()
def offline():
    """No market data: every fetch fails, so the tools fall back (to the recorded chains, to fixed values)."""
    def refuse(request):
        raise httpx.ConnectError("offline", request=request)
    saved_http, saved_home = market_data._http, os.environ.get("OPTRA_HOME")
    client = httpx.AsyncClient(transport=httpx.MockTransport(refuse))
    market_data._http = lambda slow=False: client
    home = tempfile.TemporaryDirectory()
    os.environ["OPTRA_HOME"] = home.name
    market_data._CHAIN_MEMO.clear()
    yield
    market_data._http = saved_http
    market_data._CHAIN_MEMO.clear()
    os.environ.pop("OPTRA_HOME") if saved_home is None else os.environ.__setitem__("OPTRA_HOME", saved_home)
    home.cleanup()


def undocumented(model: type[BaseModel], prefix="") -> list[str]:
    """Fields of a model (and of the models inside it) without a description."""
    missing = []
    for name, info in model.model_fields.items():
        if not info.description:
            missing.append(prefix + name)
        for arg in getattr(info.annotation, "__args__", ()) + (info.annotation,):
            if isinstance(arg, type) and issubclass(arg, BaseModel):
                missing += undocumented(arg, f"{prefix}{name}.")
    return missing


def test_every_tool_is_documented_for_a_model():
    paths = [(t.method, t.path) for t in TOOLS.values()]
    assert len(set(paths)) == len(paths) == len(TOOLS) >= 29
    for t in TOOLS.values():
        assert t.name.isidentifier() and t.title and len(t.description) >= 80, t.name
        assert undocumented(t.input) == [], (t.name, undocumented(t.input))
        t.input.model_validate(t.example)
        assert t.schema()["examples"] == [t.example]


def test_every_example_runs_and_returns_the_envelope(offline):
    made = asyncio.run(call("price_option", TOOLS["price_option"].example))["run_id"]
    for t in TOOLS.values():  # the history's examples stand in for a run: use a real one
        example = {k: made if v == RUN else v for k, v in t.example.items()}
        if t.live:  # no live engine here: it says so
            with pytest.raises(ToolError, match="live engine"):
                asyncio.run(call(t.name, example))
            continue
        r = asyncio.run(call(t.name, example))
        assert set(r) == ENVELOPE and r["status"] == "ok" and r["tool"] == t.name, t.name
        assert r["contract_version"] == CONTRACT_VERSION
        json.dumps(r, allow_nan=False)  # no NaN or infinity reaches a client
    chain = asyncio.run(call("option_chain", {"currency": "BTC"}))
    assert chain["result_summary"]["source"] == "fixture" and any("fixture" in w for w in chain["warnings"])
    assert chain["result_summary"]["chain_quality"]["options"] == chain["result_summary"]["n_options"]


def test_errors_say_what_is_wrong():
    with pytest.raises(ToolError) as e:
        asyncio.run(call("no_such_tool"))
    assert e.value.status == 404 and "price_option" in e.value.message
    ex = TOOLS["scenario_sweep"].example
    assert asyncio.run(call("scenario_sweep", {**ex, "option_type": "put"}))["warnings"] == []
    assert any("early exercise" in w for w in asyncio.run(call("scenario_sweep", {**ex, "is_american": True}))["warnings"])
    with pytest.raises(ValidationError) as v:  # a misspelt argument is an error, never silently dropped
        asyncio.run(call("compare_hedges", {**TOOLS["compare_hedges"].example, "n_path": 10}))
    assert v.value.errors()[0]["type"] == "extra_forbidden"


def test_the_engine_speaks_the_same_contract():
    assert numerics.is_engine_available() and numerics.ENGINE_PROBLEM == ""
    r = numerics.run_engine_task("pricing", {"spot": 100, "strike": 100, "rate": 0.03, "vol": 0.2, "maturity": 1})
    assert r["contract_version"] == CONTRACT_VERSION


def test_python_calls():
    r = call_sync("price_option", spot=100, strike=105, rate=0.03, vol=0.2, maturity=0.5)
    assert abs(r["result_summary"]["bs"] - 4.1783) < 1e-3

    async def inside_a_loop():
        with pytest.raises(RuntimeError):
            call_sync("price_option", spot=100)
        return await call("implied_vol", spot=100, strike=100, rate=0.03, maturity=0.5, market_price=6.5)
    assert asyncio.run(inside_a_loop())["result_summary"]["converged"]


def test_http_api():
    with TestClient(APP) as c:
        listed = c.get("/api/tools").json()
        assert [t["name"] for t in listed["tools"]] == list(TOOLS) and listed["contract_version"] == CONTRACT_VERSION
        assert all(t["input_schema"]["properties"] for t in listed["tools"] if TOOLS[t["name"]].input.model_fields)
        ex = TOOLS["price_option"].example
        by_name, by_path = c.post("/api/tools/price_option", json=ex).json(), c.post("/api/lab/pricing/run", json=ex).json()
        assert by_name["result_summary"]["bs"] == by_path["result_summary"]["bs"] and set(by_path) == ENVELOPE
        assert c.get("/api/lab/stress/packs").json()["result_summary"]["packs"]
        assert c.post("/api/tools/nope", json={}).status_code == 404
        assert c.post("/api/tools/price_option", json={"spot": -1}).status_code == 422
        assert c.post("/api/lab/screener/run", json={"strategies": {}}).json()["detail"] == "select at least one strategy type"
        assert c.get("/api/system/status").json()["numerics"] == "ok"
