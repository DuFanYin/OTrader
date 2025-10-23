import sys
from pathlib import Path
from typing import Any

# Ensure project root is on sys.path
project_root = Path(__file__).parent.parent
sys.path.insert(0, str(project_root))

from engines.engine_event import EventEngine
from engines.engine_main import MainEngine


def format_table(rows: list[tuple], headers: list[str]) -> str:
    if not rows:
        return "(no data)"

    # Convert tuples to list of strings
    str_rows: list[list[str]] = [["" if v is None else str(v) for v in row] for row in rows]

    # Ensure column count matches headers length by trimming or padding
    max_cols = len(headers)
    normalized_rows: list[list[str]] = [
        row[:max_cols] + ([""] * (max_cols - len(row))) if len(row) < max_cols else row[:max_cols]
        for row in str_rows
    ]

    # Compute column widths
    col_widths = [len(h) for h in headers]
    for row in normalized_rows:
        for i, cell in enumerate(row):
            if i < len(col_widths):
                col_widths[i] = max(col_widths[i], len(cell))

    # Build header and separator
    header_line = " | ".join(h.ljust(col_widths[i]) for i, h in enumerate(headers))
    separator = "-+-".join("-" * col_widths[i] for i in range(len(headers)))

    # Build data lines
    data_lines = [
        " | ".join(
            (row[i] if i < len(row) else "").ljust(col_widths[i]) for i in range(len(headers))
        )
        for row in normalized_rows
    ]

    return "\n".join([header_line, separator, *data_lines])


def main() -> None:
    event_engine = EventEngine()
    main_engine = MainEngine(event_engine)

    # Define expected headers aligned with CREATE TABLE schema in utilities.sql_schema
    order_headers = [
        "timestamp",
        "strategy_name",
        "orderid",
        "symbol",
        "exchange",
        "trading_class",
        "type",
        "direction",
        "price",
        "volume",
        "traded",
        "status",
        "datetime",
        "reference",
        "is_combo",
        "legs_info",
    ]

    trade_headers = [
        "timestamp",
        "strategy_name",
        "tradeid",
        "symbol",
        "exchange",
        "orderid",
        "direction",
        "price",
        "volume",
        "datetime",
    ]

    orders = main_engine.get_all_history_orders()
    trades = main_engine.get_all_history_trades()

    print("\nOrders:")
    print(format_table(orders, order_headers))

    print("\nTrades:")
    print(format_table(trades, trade_headers))

    main_engine.close()


if __name__ == "__main__":
    main()
