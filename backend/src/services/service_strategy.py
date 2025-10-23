"""Strategy service (wraps OptionStrategyEngine operations)."""

import logging
from typing import TYPE_CHECKING

from fastapi import HTTPException

from ..infrastructure.state import AppState
from .models import AddStrategyRequest, RestoreStrategyRequest

if TYPE_CHECKING:
    from engines.engine_main import MainEngine


class StrategyService:
    """Strategy service for all strategy-related operations."""

    @staticmethod
    def _get_engine() -> "MainEngine":
        engine = AppState.engine
        if engine is None:
            raise HTTPException(status_code=400, detail="system not started")
        return engine

    # ===================== Strategy Management =====================

    @staticmethod
    def list_strategies() -> dict:
        engine = AppState.engine
        if engine is None:
            return {"strategies": []}
        result = []
        for _name, strategy in engine.option_strategy_engine.strategies.items():
            result.append(strategy.get_strategy_status())
        return {"strategies": result}

    @staticmethod
    def get_strategy_updates() -> dict:
        return {"updates": AppState.strategy_updates.copy()}

    @staticmethod
    def clear_strategy_updates() -> dict:
        AppState.strategy_updates.clear()
        return {"status": "ok", "message": "Strategy updates cleared"}

    @staticmethod
    def get_strategy_classes() -> dict:
        engine = StrategyService._get_engine()
        classes = engine.option_strategy_engine.get_all_strategy_class_names()
        return {"classes": classes}

    @staticmethod
    def get_portfolios_meta() -> dict:
        engine = StrategyService._get_engine()
        names = engine.market_data_engine.get_all_portfolio_names()
        return {"portfolios": names}

    @staticmethod
    def get_removed_strategies() -> dict:
        engine = StrategyService._get_engine()
        names = engine.option_strategy_engine.get_removed_strategies()
        return {"removed_strategies": names}

    # ===================== Strategy Lifecycle =====================

    @staticmethod
    def add_strategy(req: AddStrategyRequest) -> dict:
        engine = StrategyService._get_engine()
        try:
            engine.option_strategy_engine.add_strategy(req.strategy_class, req.portfolio_name, req.setting)
            strategy_name = f"{req.strategy_class}_{req.portfolio_name}"
            return {"status": "ok", "strategy_name": strategy_name}
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e)) from e

    @staticmethod
    def restore_strategy(req: RestoreStrategyRequest) -> dict:
        engine = StrategyService._get_engine()
        try:
            engine.option_strategy_engine.recover_strategy(req.strategy_name)
            return {"status": "ok"}
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e)) from e

    @staticmethod
    def init_strategy(strategy_name: str) -> dict:
        engine = StrategyService._get_engine()
        try:
            engine.option_strategy_engine.init_strategy(strategy_name)
            return {"status": "ok"}
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e)) from e

    @staticmethod
    def start_strategy(strategy_name: str) -> dict:
        engine = StrategyService._get_engine()
        try:
            engine.option_strategy_engine.start_strategy(strategy_name)
            return {"status": "ok"}
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e)) from e

    @staticmethod
    def stop_strategy(strategy_name: str) -> dict:
        engine = StrategyService._get_engine()
        try:
            engine.option_strategy_engine.stop_strategy(strategy_name)
            return {"status": "ok"}
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e)) from e

    @staticmethod
    def remove_strategy(strategy_name: str) -> dict:
        engine = StrategyService._get_engine()
        try:
            removed = engine.option_strategy_engine.remove_strategy(strategy_name)
            return {"status": "ok", "removed": bool(removed)}
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e)) from e

    @staticmethod
    def delete_strategy(strategy_name: str) -> dict:
        engine = StrategyService._get_engine()
        try:
            deleted = engine.option_strategy_engine.delete_strategy(strategy_name)
            return {"status": "ok", "deleted": bool(deleted)}
        except Exception as e:
            raise HTTPException(status_code=400, detail=str(e)) from e

    # ===================== Strategy Holdings =====================

    @staticmethod
    def get_strategy_holdings() -> dict:
        try:
            engine = StrategyService._get_engine()
            # Build a dict of strategy_name -> serialized holding
            result: dict[str, dict] = {}
            pe = engine.option_strategy_engine.position_engine

            # Debug: Log available strategies and holdings
            strategies = list(engine.option_strategy_engine.strategies.keys())
            holdings_keys = list(pe.strategy_holdings.keys())
            logging.info(f"Available strategies: {strategies}")
            logging.info(f"Available holdings: {holdings_keys}")

            # If no strategies exist, return empty holdings with debug info
            if not strategies:
                logging.warning("No strategies found in the system")
                return {"holdings": {}, "debug": {"strategies": strategies, "holdings": holdings_keys}}

            # If strategies exist but no holdings, return debug info
            if not holdings_keys:
                logging.warning(f"Strategies exist ({strategies}) but no holdings found")
                return {"holdings": {}, "debug": {"strategies": strategies, "holdings": holdings_keys}}

            for name in pe.strategy_holdings.keys():
                try:
                    serialized = pe.serialize_holding(name)
                    result[name] = serialized
                    logging.info(
                        f"Serialized holding for {name}: {len(serialized.get('options', []))} options, {len(serialized.get('combos', []))} combos"
                    )
                except Exception as e:
                    logging.exception("Failed to serialize holding for %s: %s", name, str(e))
                    continue
            return {"holdings": result, "debug": {"strategies": strategies, "holdings": holdings_keys}}
        except HTTPException:
            # Engine not started
            logging.warning("Engine not started - returning empty holdings")
            return {"holdings": {}, "error": "Engine not started"}
        except Exception as e:
            logging.exception("Error getting strategy holdings: %s", str(e))
            return {"error": str(e)}
