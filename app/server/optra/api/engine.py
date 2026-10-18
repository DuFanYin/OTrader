"""The trading engines' routes: the gateway and market data, the account, risk, contracts, strategies, holdings, the
log. Each is one call of an engine's client (infra/live.py), the paper account's unless ?account=live names the live
one's; the gateway's and market data's state come from the status the server polls, without a call; the log and the
state the Home view shows are streams the server keeps for every page at once."""

from datetime import date

from fastapi import APIRouter, Body, Depends, HTTPException, Query, WebSocket
from fastapi.responses import StreamingResponse

from optra.infra import access, live
from optra.infra.remote_client import EngineClient
from optra.schemas.engine import AddStrategyRequest, RestoreStrategyRequest

router = APIRouter()
ANYONE = [Depends(access.anyone)]
PERSON = [Depends(access.person_only)]  # what changes the engine is the person's; halting is anyone's


@router.post("/api/gateway/connect", dependencies=PERSON)
def connect_gateway(engine: EngineClient = Depends(live.engine)) -> dict:
    return engine.connect_gateway()


@router.post("/api/gateway/disconnect", dependencies=PERSON)
def disconnect_gateway(engine: EngineClient = Depends(live.engine)) -> dict:
    return engine.disconnect_gateway()


@router.get("/api/gateway/status", dependencies=ANYONE)
def get_gateway_status(account: str = Query("paper")) -> dict:
    return live.flag("ib", account)


@router.get("/api/market/status", dependencies=ANYONE)
def get_market_status(account: str = Query("paper")) -> dict:
    return live.flag("md", account)


@router.post("/api/market/start", dependencies=PERSON)
def start_market(engine: EngineClient = Depends(live.engine)) -> dict:
    return engine.start_market_data()


@router.post("/api/market/stop", dependencies=PERSON)
def stop_market(engine: EngineClient = Depends(live.engine)) -> dict:
    return engine.stop_market_data()


@router.get("/api/account", dependencies=ANYONE)
def get_account(engine: EngineClient = Depends(live.engine)) -> dict:
    """The account at IB beside the strategies' holdings, the last recorded broker check, unsettled options: read
    only (POST /api/account/check-with-broker records a check)."""
    return engine.get_account()


@router.post("/api/account/check-with-broker", dependencies=PERSON)
def check_with_broker(engine: EngineClient = Depends(live.engine)) -> dict:
    """Check the strategies' holdings against IB's positions again (after setting them right)."""
    return engine.check_with_broker()


@router.post("/api/account/settle", dependencies=PERSON)
def settle_expired(
    portfolio: str = Body(...), expiry: date = Body(...), price: float = Body(..., gt=0),
    engine: EngineClient = Depends(live.engine),
) -> dict:
    """Settle the portfolio's options that expired on expiry at the underlying's closing price."""
    return engine.settle_expired(portfolio, expiry.isoformat(), price)


@router.post("/api/account/book", dependencies=PERSON)
def book_manually(
    strategy: str = Body(...), symbol: str = Body(...), side: str = Body(..., pattern="^(BUY|SELL)$"),
    quantity: float = Body(..., gt=0), price: float = Body(..., ge=0), reason: str = Body(..., min_length=1),
    engine: EngineClient = Depends(live.engine),
) -> dict:
    """Book a fill the engine missed (the broker check disagrees and IB is right)."""
    return engine.book_manually(strategy, symbol, side, quantity, price, reason)


@router.get("/api/risk", dependencies=ANYONE)
def get_risk(engine: EngineClient = Depends(live.engine)) -> dict:
    """The limits every order is checked against, the halt, the orders refused."""
    return engine.get_risk()


@router.post("/api/risk/limits", dependencies=PERSON)
def set_risk_limits(values: dict[str, float] = Body(..., embed=True),
                    engine: EngineClient = Depends(live.engine)) -> dict:
    """Set risk limits by name; saved by the engine."""
    return engine.set_risk_limits(values)


@router.post("/api/risk/halt", dependencies=ANYONE)
def halt(reason: str = Body("", embed=True), engine: EngineClient = Depends(live.engine)) -> dict:
    """Halt trading: every strategy stopped, working orders cancelled, only closing orders go."""
    return engine.halt(reason)


@router.post("/api/risk/clear", dependencies=PERSON)
def clear_halt(engine: EngineClient = Depends(live.engine)) -> dict:
    """Clear the halt: strategies may be started again."""
    return engine.clear_halt()


@router.post("/api/contracts/sync", dependencies=PERSON)
def sync_contracts(
    portfolio: str = Body(...), max_dte: int = Body(7, ge=0, le=60), engine: EngineClient = Depends(live.engine)
) -> dict:
    """The portfolio's contracts from IB, rolled into the running engine at once (as it does each
    trading morning)."""
    return engine.sync_contracts(portfolio, max_dte)


@router.get("/api/strategies", dependencies=ANYONE)
def list_strategies(engine: EngineClient = Depends(live.engine)) -> dict:
    return {"strategies": engine.list_strategies()}


@router.get("/api/strategies/meta/portfolios", dependencies=ANYONE)
def get_portfolios(engine: EngineClient = Depends(live.engine)) -> dict:
    return {"portfolios": engine.get_portfolio_names()}



@router.post("/api/strategies", dependencies=PERSON)
def add_strategy(req: AddStrategyRequest, engine: EngineClient = Depends(live.engine)) -> dict:
    return engine.add_strategy(req.strategy_class, req.portfolio_name, req.setting, req.strategy_name)


@router.post("/api/strategies/restore", dependencies=PERSON)
def restore_strategy(req: RestoreStrategyRequest, engine: EngineClient = Depends(live.engine)) -> dict:
    return engine.restore_strategy(req.strategy_name)


@router.post("/api/strategies/{strategy_name}/init", dependencies=PERSON)
def init_strategy(strategy_name: str, engine: EngineClient = Depends(live.engine)) -> dict:
    return engine.init_strategy(strategy_name)


@router.post("/api/strategies/{strategy_name}/start", dependencies=PERSON)
def start_strategy(strategy_name: str, engine: EngineClient = Depends(live.engine)) -> dict:
    return engine.start_strategy(strategy_name)


@router.post("/api/strategies/{strategy_name}/stop", dependencies=PERSON)
def stop_strategy(strategy_name: str, engine: EngineClient = Depends(live.engine)) -> dict:
    return engine.stop_strategy(strategy_name)


@router.delete("/api/strategies/{strategy_name}/remove", dependencies=PERSON)
def remove_strategy(strategy_name: str, engine: EngineClient = Depends(live.engine)) -> dict:
    return engine.remove_strategy(strategy_name)


@router.delete("/api/strategies/{strategy_name}/delete", dependencies=PERSON)
def delete_strategy(strategy_name: str, engine: EngineClient = Depends(live.engine)) -> dict:
    return engine.delete_strategy(strategy_name)


@router.get("/api/strategies/holdings", dependencies=ANYONE)
def get_strategy_holdings(engine: EngineClient = Depends(live.engine)) -> dict:
    return engine.get_strategy_holdings()


@router.get("/api/live/stream", dependencies=ANYONE)
async def live_stream(account: str = Query("paper")) -> StreamingResponse:
    """The strategies, their holdings and the risk state as Server-Sent Events: now, then each time they change. One
    reader of the engine serves every page watching (infra/live.py)."""
    if (link := live.link(account)) is None:
        raise HTTPException(503, f"the server runs without the {account} account's engine")
    return StreamingResponse(link.watch_state(), media_type="text/event-stream",
                             headers={"Cache-Control": "no-store", "X-Accel-Buffering": "no"})


@router.get("/api/data/portfolios", dependencies=ANYONE)
def get_portfolio_names(engine: EngineClient = Depends(live.engine)) -> dict:
    return {"portfolios": engine.get_portfolio_names()}


@router.get("/api/logs", dependencies=ANYONE)
def get_logs(limit: int = Query(200, ge=1, le=live.RECENT_LINES), account: str = Query("paper")) -> dict:
    """The engine's recent log lines."""
    link = live.link(account)
    return {"logs": list(link.lines)[-limit:] if link else []}


@router.post("/api/logs/clear", dependencies=PERSON)
def clear_logs(account: str = Query("paper")) -> dict:
    """Forget the recent lines (the page's Clear)."""
    if (link := live.link(account)) is not None:
        link.lines.clear()
    return {"status": "ok"}


@router.websocket("/ws/logs")
async def logs_socket(ws: WebSocket, account: str = "paper") -> None:
    """The recent lines, then each as the engine writes it."""
    if (link := live.link(account)) is None:
        await ws.close(code=1013)
        return
    await link.serve_logs(ws)
