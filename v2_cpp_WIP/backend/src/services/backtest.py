"""Backtest service: parameter validation, process management, and C++ runner bridge."""

from __future__ import annotations

from typing import Any, Dict

from src.infra.backtest_runner import cancel_current_backtest, run_backtest_cpp
from src.infra.state import AppState


DEFAULT_FEE_RATE = 0.35
DEFAULT_RISK_FREE_RATE = 0.05
DEFAULT_IV_PRICE_MODE = "mid"


class BacktestService:
    """High-level backtest operations."""

    @staticmethod
    def run_backtest(request: Dict[str, Any]) -> Dict[str, Any]:
        parquet = request.get("parquet", "")
        strategy = request.get("strategy", "")
        strategy_setting = request.get("strategy_setting", None)
        iv_price_mode = str(request.get("iv_price_mode", DEFAULT_IV_PRICE_MODE) or DEFAULT_IV_PRICE_MODE).lower()
        start_date = request.get("start_date")  # YYYY-MM-DD format
        end_date = request.get("end_date")  # YYYY-MM-DD format
        try:
            fee_rate = float(request.get("fee_rate", DEFAULT_FEE_RATE) or DEFAULT_FEE_RATE)
            slippage_bps = float(request.get("slippage_bps", 5.0) or 5.0)
            risk_free_rate = float(request.get("risk_free_rate", DEFAULT_RISK_FREE_RATE) or DEFAULT_RISK_FREE_RATE)
        except (TypeError, ValueError):
            return {"status": "error", "error": "fee_rate, slippage_bps and risk_free_rate must be numbers"}
        if iv_price_mode not in {"mid", "bid", "ask"}:
            return {"status": "error", "error": "iv_price_mode must be mid, bid, or ask"}
        if fee_rate < 0:
            return {"status": "error", "error": "fee_rate must be >= 0"}
        if slippage_bps < 0:
            slippage_bps = 0.0
        if not parquet or not strategy:
            return {"status": "error", "error": "Missing required fields: parquet, strategy"}
        if not start_date or not end_date:
            return {"status": "error", "error": "Missing required fields: start_date, end_date"}

        result = run_backtest_cpp(
            parquet_path=parquet,
            strategy_name=strategy,
            fee_rate=fee_rate,
            slippage_bps=slippage_bps,
            risk_free_rate=risk_free_rate,
            iv_price_mode=iv_price_mode,
            strategy_setting=strategy_setting,
            start_date=start_date,
            end_date=end_date,
        )
        # progress_info into result
        if result.get("status") == "ok" and "result" in result and "progress_info" in result:
            result["result"]["progress_info"] = result.pop("progress_info")
        return result

    @staticmethod
    def cancel_backtest() -> Dict[str, Any]:
        """Cancel running C++ backtest (if any)."""
        return cancel_current_backtest()

