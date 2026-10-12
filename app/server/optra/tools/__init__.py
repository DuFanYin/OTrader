"""Every tool, by name, in the order they are listed: the numerics (pricing, Greeks, calibration, stress, hedging,
market data, the screener, the theory checks), the trading engine's backtests, and the run history.

call runs one inside an event loop (the API); call_sync runs one from plain code (the tests)."""

import asyncio

from optra.tools import (calibration, engine, greeks, hedging, market, pricing, runs, screener, strategy, stress,
                         theory)
from optra.tools.base import Result, Tool, ToolError, run_tool

TOOLS: dict[str, Tool] = {t.name: t for module in (pricing, strategy, greeks, calibration, stress, hedging, market,
                                                    screener, theory, engine, runs) for t in module.TOOLS}


async def call(name: str, arguments: dict | None = None, /, **kwargs) -> dict:
    """Runs the tool `name` on `arguments` (validated against its input model) and returns the envelope.
    Raises ToolError for an unknown tool or one that cannot answer, pydantic.ValidationError for bad arguments."""
    tool = TOOLS.get(name)
    if tool is None:
        raise ToolError(f"no tool named {name!r}; the tools are: {', '.join(TOOLS)}", 404)
    return await run_tool(tool, tool.input.model_validate({**(arguments or {}), **kwargs}))


def call_sync(name: str, arguments: dict | None = None, /, **kwargs) -> dict:
    """call, waited for, outside an event loop."""
    try:
        asyncio.get_running_loop()
    except RuntimeError:
        pass
    else:
        raise RuntimeError("call_sync cannot run inside an event loop: await call(...) instead")

    async def run() -> dict:
        from optra.services import market_data
        try:
            return await call(name, arguments, **kwargs)
        finally:
            await market_data.close()
    return asyncio.run(run())


__all__ = ["TOOLS", "Result", "Tool", "ToolError", "call", "call_sync"]
