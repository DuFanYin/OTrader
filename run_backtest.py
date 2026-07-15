#!/usr/bin/env python3
"""
Run backtest from repo root: call C++ engine via backend logic, print metrics, save chart.
Run from repo root: python run_backtest.py
"""
from __future__ import annotations

import sys
from pathlib import Path
from datetime import datetime, timezone
try:
    from zoneinfo import ZoneInfo
except ImportError:  # pragma: no cover - Python<3.9 fallback
    ZoneInfo = None

# Ensure repo root and backend are on path (backend code uses "from src.xxx")
_REPO_ROOT = Path(__file__).resolve().parent
_BACKEND = _REPO_ROOT / "backend"
for p in (_REPO_ROOT, _BACKEND):
    if str(p) not in sys.path:
        sys.path.insert(0, str(p))

from backend.src.infra.backtest_runner import run_backtest_cpp
from backend.src.utils.chart import render_backtest_chart_svg

# --- Hardcoded parameters (edit here) ---
SYMBOL = "SPXW"
START_DATE = "2025-04-24"
END_DATE = "2025-05-24"
STRATEGY_NAME = "StraddleInventoryScalperStrategy"
FEE_RATE = 0.35
SLIPPAGE_BPS = 5.0
RISK_FREE_RATE = 0.05
IV_PRICE_MODE = "mid"
CHART_OUT = ".vscode/backtest_chart.svg"
# -----------------------------------------


def main() -> int:
    payload = run_backtest_cpp(
        parquet_path=SYMBOL,
        strategy_name=STRATEGY_NAME,
        fee_rate=FEE_RATE,
        slippage_bps=SLIPPAGE_BPS,
        risk_free_rate=RISK_FREE_RATE,
        iv_price_mode=IV_PRICE_MODE,
        strategy_setting=None,
        start_date=START_DATE,
        end_date=END_DATE,
    )

    if payload.get("status") != "ok":
        print("Backtest failed:", payload.get("error", "unknown"), file=sys.stderr)
        return 1

    result = payload.get("result") or {}
    final_pnl = result.get("final_pnl", 0)
    total_fees = result.get("total_fees", 0)
    net_pnl = result.get("net_pnl", final_pnl - total_fees)
    total_orders = result.get("total_orders", 0)
    max_drawdown = result.get("max_drawdown")
    daily_sharpe = result.get("daily_sharpe")
    num_days = result.get("num_days", 0)
    processed_timesteps = result.get("processed_timesteps", 0)
    total_timesteps = result.get("total_timesteps", 0)
    total_frames = result.get("total_frames")
    total_rows = result.get("total_rows", 0)
    max_delta = result.get("max_delta")
    max_gamma = result.get("max_gamma")
    max_theta = result.get("max_theta")
    duration_seconds = result.get("duration_seconds")
    duration_ms = result.get("duration_ms")
    start_time = result.get("start_time", "")
    end_time = result.get("end_time", "")

    def _fmt_utc_et(ts: str) -> tuple[str, str]:
        if not ts:
            return "", ""
        try:
            dt_utc = datetime.strptime(ts, "%Y-%m-%dT%H:%M:%S").replace(tzinfo=timezone.utc)
        except Exception:
            return ts, ""
        if ZoneInfo is None:
            return dt_utc.isoformat(), ""
        et = dt_utc.astimezone(ZoneInfo("America/New_York"))
        return dt_utc.strftime("%Y-%m-%dT%H:%M:%S"), et.strftime("%Y-%m-%dT%H:%M:%S")

    print("--- Metrics ---")
    start_utc, start_et = _fmt_utc_et(start_time)
    end_utc, end_et = _fmt_utc_et(end_time)
    if start_utc:
        print(f"  Start (UTC):  {start_utc}")
        if start_et:
            print(f"  Start (ET):   {start_et}")
    else:
        print(f"  Start:        {start_time}")
    if end_utc:
        print(f"  End   (UTC):  {end_utc}")
        if end_et:
            print(f"  End   (ET):   {end_et}")
    else:
        print(f"  End:          {end_time}")
    print(f"  Final PnL:    {final_pnl:.2f}")
    print(f"  Total fees:   {total_fees:.2f}")
    print(f"  Net PnL:      {net_pnl:.2f}")
    print(f"  Total orders: {total_orders}")
    print(f"  Num days:     {num_days}")
    if max_drawdown is not None:
        print(f"  Max drawdown: {max_drawdown:.2f}")
    if daily_sharpe is not None:
        print(f"  Daily Sharpe: {daily_sharpe:.3f}")
    if max_delta is not None:
        print(f"  Max delta:    {max_delta:.4f}")
    if max_gamma is not None:
        print(f"  Max gamma:    {max_gamma:.4f}")
    if max_theta is not None:
        print(f"  Max theta:    {max_theta:.4f}")
    print(f"  Timesteps:    {processed_timesteps} / {total_timesteps}")
    if total_frames is not None:
        print(f"  Total frames: {total_frames}")
    print(f"  Total rows:   {total_rows:,}")
    if duration_seconds is not None:
        if duration_seconds >= 1:
            print(f"  Duration:     {duration_seconds:.2f}s")
        else:
            print(f"  Duration:     {(duration_ms or 0):.0f}ms")

    chart_path = Path(CHART_OUT)
    svg_content = payload.get("chart_svg")
    if not svg_content:
        try:
            if payload.get("chart_data"):
                svg_content = render_backtest_chart_svg(chart_data=payload["chart_data"])
        except Exception as e:
            print(f"Chart render failed: {e}", file=sys.stderr)
    if svg_content:
        chart_path.write_text(svg_content, encoding="utf-8")
        print(f"Chart saved: {chart_path}")
    else:
        print("No chart data (no timestep_metrics or render failed).", file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
