from __future__ import annotations

import json
import subprocess
from datetime import datetime
from pathlib import Path
from typing import Any

from src.infra.state import AppState


def _find_matching_files(
    symbol: str,
    start_date: str | None = None,
    end_date: str | None = None,
) -> list[Path]:
    """
    Find all parquet files matching the symbol and date range.
    Files are named YYYYMMDD.parquet and located under data/{symbol}/.
    Returns sorted list of file paths (relative to project root).
    """
    root = Path.cwd()
    if root.name == "build":
        root = root.parent

    symbol_dir = root / "data" / symbol
    if not symbol_dir.exists() or not symbol_dir.is_dir():
        return []

    # Find all parquet files recursively
    all_files: list[Path] = []
    for p in symbol_dir.rglob("*.parquet"):
        if p.is_file():
            all_files.append(p)

    # Filter by date range if provided
    if start_date or end_date:
        filtered: list[Path] = []
        for p in all_files:
            # Extract date from filename (YYYYMMDD.parquet)
            stem = p.stem
            if len(stem) == 8 and stem.isdigit():
                file_date_str = stem  # YYYYMMDD
                try:
                    file_date = datetime.strptime(file_date_str, "%Y%m%d").date()
                    if start_date:
                        start_dt = datetime.strptime(start_date.replace("-", ""), "%Y%m%d").date()
                        if file_date < start_dt:
                            continue
                    if end_date:
                        end_dt = datetime.strptime(end_date.replace("-", ""), "%Y%m%d").date()
                        if file_date > end_dt:
                            continue
                    filtered.append(p)
                except ValueError:
                    # Skip files with invalid date format
                    continue
        all_files = filtered

    # Sort by filename (which contains date)
    all_files.sort(key=lambda p: p.stem)

    # Convert to relative paths
    return [p.relative_to(root) for p in all_files]


def _extract_json_from_mixed_stdout(stdout: str) -> dict[str, Any]:
    """
    Parse JSON payload from mixed stdout that may contain C++ logs.
    """
    text = (stdout or "").strip()
    if not text:
        raise ValueError("empty stdout")

    # Fast path: pure JSON
    try:
        return json.loads(text)
    except Exception:
        pass

    # Fallback: C++ logs may be printed before the final JSON object.
    marker = '{"status"'
    pos = text.rfind(marker)
    if pos >= 0:
        candidate = text[pos:].strip()
        return json.loads(candidate)

    raise ValueError("no JSON payload marker found")


def run_backtest_cpp(
    parquet_path: str,
    strategy_name: str,
    fee_rate: float = 0.35,
    slippage_bps: float = 5.0,
    risk_free_rate: float = 0.05,
    iv_price_mode: str = "mid",
    strategy_setting: dict[str, Any] | None = None,
    start_date: str | None = None,
    end_date: str | None = None,
) -> dict[str, Any]:
    """
    Run backtest via C++ executable; return JSON dict.
    When start_date/end_date given: parquet_path = symbol, filter under data/{symbol}/.
    Otherwise: parquet_path = single file path.
    """
    root = Path.cwd()
    if root.name == "build":
        root = root.parent

    parquet_files: list[str] = []

    if start_date or end_date:
        # parquet_path as symbol (last path segment)
        symbol = (parquet_path.strip().split("/")[-1]) or parquet_path.strip()
        matching_files = _find_matching_files(symbol, start_date, end_date)
        if not matching_files:
            return {
                "status": "error",
                "error": f"No files found for symbol {symbol} in date range {start_date} to {end_date}",
            }
        parquet_files = [str(f) for f in matching_files]
    else:
        parquet_files = [parquet_path]

    for f in parquet_files:
        parquet_full = (root / f).resolve()
        if not parquet_full.is_file():
            return {"status": "error", "error": f"Parquet file not found: {f}"}

    runner = (root / "Otrader" / "build" / "entry_backtest").resolve()
    if not runner.is_file():
        return {
            "status": "error",
            "error": (
                "C++ runner not found. Build it first: "
                "cmake -S Otrader -B Otrader/build && "
                "cmake --build Otrader/build --target entry_backtest"
            ),
        }

    args = [str(runner)]

    # Build file arguments
    if len(parquet_files) == 1:
        # Single file: use old format
        args.append(parquet_files[0])
    else:
        # Multiple files: use --files flag
        args.append("--files")
        args.extend(parquet_files)

    args.append(strategy_name)
    args.extend(
        [
            "--fee-rate",
            str(float(fee_rate)),
            "--slippage-bps",
            str(float(slippage_bps)),
            "--risk-free-rate",
            str(float(risk_free_rate)),
            "--iv-price-mode",
            str(iv_price_mode),
        ]
    )
    if strategy_setting:
        for key, value in strategy_setting.items():
            if isinstance(value, (int, float)):
                args.append(f"{key}={float(value)}")

    # If a previous backtest is still running, terminate it first
    prev = AppState.backtest.backtest_proc
    if prev is not None and prev.poll() is None:
        try:
            prev.terminate()
        except Exception:
            try:
                prev.kill()
            except Exception:
                pass
    AppState.backtest.backtest_proc = None

    try:
        proc = subprocess.Popen(
            args,
            cwd=str(root),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        AppState.backtest.backtest_proc = proc
        stdout, stderr = proc.communicate()
    except Exception as exc:
        AppState.backtest.backtest_proc = None
        return {"status": "error", "error": f"Failed to execute C++ runner: {exc}"}
    finally:
        AppState.backtest.backtest_proc = None

    stdout = (stdout or "").strip()
    stderr = (stderr or "").strip()

    # Cancelled (Stop/refresh): no output; return cancelled for frontend
    if proc.returncode not in (0, None) and not stdout and not stderr:
        return {"status": "cancelled", "error": "Backtest cancelled by user"}

    # Extract progress information from stderr if available
    progress_info = None
    if stderr:
        for line in stderr.split("\n"):
            if line.strip().startswith('{"type":"progress"'):
                try:
                    progress_info = json.loads(line.strip())
                except json.JSONDecodeError:
                    pass
    if not stdout:
        msg = "C++ runner returned empty output"
        if stderr:
            msg = f"{msg}. stderr: {stderr}"
        return {"status": "error", "error": msg}

    try:
        payload = _extract_json_from_mixed_stdout(stdout)
    except Exception:
        snippet = stdout[:400]
        msg = f"C++ runner output is not valid JSON: {snippet}"
        if stderr:
            msg = f"{msg}. stderr: {stderr[:200]}"
        return {"status": "error", "error": msg}

    if proc.returncode != 0 and payload.get("status") != "error":
        return {
            "status": "error",
            "error": payload.get("error") or f"C++ runner failed with code {proc.returncode}",
        }

    # Add progress info to payload if available
    if progress_info:
        payload["progress_info"] = progress_info

    # Backend-rendered chart: SVG (vector, sharp at any zoom). C++ prepares chart_data.
    if payload.get("status") == "ok" and payload.get("chart_data"):
        from src.utils.chart import (
            render_backtest_chart_base64,
            render_backtest_chart_svg,
        )

        try:
            payload["chart_svg"] = render_backtest_chart_svg(chart_data=payload["chart_data"])
        except Exception as e_svg:  # noqa: BLE001
            # Fallback to PNG base64
            try:
                payload["chart_image_base64"] = render_backtest_chart_base64(
                    chart_data=payload["chart_data"]
                )
                payload.setdefault("errors", []).append(
                    f"chart_svg_failed: {type(e_svg).__name__}: {e_svg}"
                )
            except Exception as e_png:  # noqa: BLE001
                # Do not fail the whole backtest; just record the error
                payload.setdefault("errors", []).append(
                    f"chart_render_failed: svg={type(e_svg).__name__}: {e_svg}; "
                    f"png={type(e_png).__name__}: {e_png}"
                )
                # Log to stderr
                import sys

                print(
                    f"[backtest] chart rendering failed: svg={e_svg!r}, png={e_png!r}",
                    file=sys.stderr,
                )

    return payload


def cancel_current_backtest() -> dict[str, Any]:
    """
    Cancel currently running C++ backtest process (if any).
    Used when user refreshes/leaves backtest page.
    """
    proc = AppState.backtest.backtest_proc
    if proc is None or proc.poll() is not None:
        AppState.backtest.backtest_proc = None
        return {"status": "ok", "message": "no active backtest"}

    try:
        proc.terminate()
    except Exception:
        try:
            proc.kill()
        except Exception:
            pass
    finally:
        AppState.backtest.backtest_proc = None

    return {"status": "ok", "message": "backtest cancelled"}

