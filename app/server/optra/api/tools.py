"""The tools: every computation Optra offers, declared once (optra.tools) with a described input model, and
reached here over HTTP: a route each at its path under /api, /api/tools to list them with their schemas and to run
one by name, and the runs they keep at /api/runs."""

from typing import Annotated

from fastapi import APIRouter, Body, Query, Request

from optra.infra import run_store
from optra.tools import TOOLS, Tool, ToolError, call, run_tool
from optra.version import CONTRACT_VERSION

router = APIRouter()


def _endpoint(tool: Tool):
    if "{" in tool.path:  # path parameters (get_run): the model is read from the path and the query together
        async def endpoint(request: Request) -> dict:
            return await run_tool(tool, tool.input.model_validate({**request.query_params, **request.path_params}))
    elif tool.method == "GET":
        async def endpoint(params: Annotated[tool.input, Query()]) -> dict:
            return await run_tool(tool, params)
    else:
        async def endpoint(params: tool.input) -> dict:
            return await run_tool(tool, params)
    return endpoint


def tool_info(t: Tool) -> dict:
    return {"name": t.name, "title": t.title, "description": t.description, "cost": t.cost, "network": t.network,
            "http": {"method": t.method, "path": "/api" + t.path}, "input_schema": t.schema()}


@router.get("/api/tools", tags=["tools"])
def list_tools() -> dict:
    """Every tool, with its description and input schema."""
    return {"contract_version": CONTRACT_VERSION, "tools": [tool_info(t) for t in TOOLS.values()]}


@router.post("/api/tools/{name}", tags=["tools"])
async def call_tool(name: str, arguments: Annotated[dict, Body(default_factory=dict)]) -> dict:
    """Runs one tool by name; the body is its arguments."""
    return await call(name, arguments)


@router.delete("/api/runs/{run_id}", tags=["tools"])
def delete_run(run_id: str) -> dict:
    """Removes a run from the history."""
    if not run_store.store().delete(run_id):
        raise ToolError(f"no run {run_id}", 404)
    return {"deleted": run_id}


for _tool in TOOLS.values():  # after the fixed routes: /api/tools/{name} must not be taken for a tool's path
    router.add_api_route("/api" + _tool.path, _endpoint(_tool), methods=[_tool.method], name=_tool.name,
                         summary=_tool.title, description=_tool.description, tags=["tools"])
