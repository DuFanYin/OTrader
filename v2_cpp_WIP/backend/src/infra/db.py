from __future__ import annotations

import os
from typing import Any, Dict, List, Optional, Sequence, Tuple

try:
    import psycopg2  # type: ignore[import]
except Exception:  # pragma: no cover - optional dependency
    psycopg2 = None


class DBError(Exception):
    """Domain-specific error for database access issues."""


def get_connection():
    """PostgreSQL read-only connection (DATABASE_URL)."""
    if psycopg2 is None:
        raise DBError("psycopg2 is not installed on backend")
    conninfo = os.environ.get("DATABASE_URL", "").strip()
    if not conninfo:
        raise DBError("DATABASE_URL not configured")
    return psycopg2.connect(conninfo)


def fetch_orders_trades_raw(
    strategy: Optional[str],
    limit: int,
) -> Tuple[List[str], Sequence[Tuple[Any, ...]]]:
    """
    Low-level query for historical orders & trades.

    Returns a tuple of:
    - strategies: distinct strategy_name list (for filters)
    - rows: raw rows from the unified orders/trades query
    """
    conn = get_connection()
    try:
        with conn.cursor() as cur:
            # Distinct strategy names
            cur.execute(
                """
                SELECT DISTINCT strategy_name FROM orders
                UNION
                SELECT DISTINCT strategy_name FROM trades
                ORDER BY strategy_name
                """
            )
            strategies = [row[0] for row in cur.fetchall() if row[0]]

            params: List[Any] = []
            where_clauses_orders: List[str] = []
            where_clauses_trades: List[str] = []

            if strategy:
                where_clauses_orders.append("strategy_name = %s")
                where_clauses_trades.append("strategy_name = %s")
                params.append(strategy)

            orders_sql = """
                SELECT
                    'Order' AS record_type,
                    timestamp,
                    strategy_name,
                    orderid,
                    symbol,
                    direction,
                    price,
                    volume,
                    traded,
                    status
                FROM orders
            """
            trades_sql = """
                SELECT
                    'Trade' AS record_type,
                    timestamp,
                    strategy_name,
                    tradeid,
                    symbol,
                    direction,
                    price,
                    volume,
                    NULL::double precision AS traded,
                    NULL::text AS status
                FROM trades
            """

            if strategy:
                if where_clauses_orders:
                    orders_sql += " WHERE " + " AND ".join(where_clauses_orders)
                if where_clauses_trades:
                    trades_sql += " WHERE " + " AND ".join(where_clauses_trades)

            union_sql = f"""
                {orders_sql}
                UNION ALL
                {trades_sql}
                ORDER BY timestamp DESC
                LIMIT %s
            """

            cur.execute(union_sql, (*params, limit))
            rows = cur.fetchall()

        return strategies, rows
    except Exception as exc:  # pragma: no cover - simple wrapper
        raise DBError(f"Failed to fetch orders/trades: {exc}") from exc
    finally:
        conn.close()


def fetch_contracts_summary() -> Dict[str, Any]:
    """Summary of contract_equity and contract_option (equity/option totals + rows)."""
    conn = get_connection()
    try:
        with conn.cursor() as cur:
            # Equity
            cur.execute("SELECT COUNT(*) FROM contract_equity")
            equity_total_row = cur.fetchone()
            equity_total = int(equity_total_row[0]) if equity_total_row else 0

            cur.execute(
                """
                SELECT symbol, exchange, product, size, pricetick, gateway_name
                FROM contract_equity
                ORDER BY symbol
                LIMIT 50
                """
            )
            equity_rows = [
                {
                    "symbol": row[0],
                    "exchange": row[1],
                    "product": row[2],
                    "size": float(row[3]) if row[3] is not None else None,
                    "pricetick": float(row[4]) if row[4] is not None else None,
                    "gateway_name": row[5],
                }
                for row in cur.fetchall()
            ]

            # Option totals + chains
            cur.execute(
                """
                SELECT
                    COUNT(*) AS cnt,
                    COUNT(
                        DISTINCT
                        split_part(symbol, '-', 1) || '_' || split_part(symbol, '-', 2)
                    ) AS chains
                FROM contract_option
                """
            )
            opt_row = cur.fetchone()
            option_total = int(opt_row[0]) if opt_row and opt_row[0] is not None else 0
            option_chains = int(opt_row[1]) if opt_row and opt_row[1] is not None else 0

            # Per-chain (DTE, strike range)
            cur.execute(
                """
                WITH opt AS (
                    SELECT
                        split_part(symbol, '-', 1) || '_' || split_part(symbol, '-', 2) AS chain_symbol,
                        expiry,
                        type,
                        strike,
                        underlying
                    FROM contract_option
                )
                SELECT
                    chain_symbol,
                    MIN(DATE(expiry)) AS expiry_date,
                    GREATEST(0, MIN(DATE(expiry) - CURRENT_DATE)) AS dte,
                    COUNT(*) FILTER (WHERE type = 'CALL') AS calls,
                    COUNT(*) FILTER (WHERE type = 'PUT') AS puts,
                    MIN(strike) AS strike_min,
                    MAX(strike) AS strike_max,
                    MIN(underlying) AS underlying_sample
                FROM opt
                GROUP BY chain_symbol
                ORDER BY expiry_date, chain_symbol
                LIMIT 100
                """
            )
            option_rows = [
                {
                    "chain": row[0],
                    "dte": int(row[2]) if row[2] is not None else None,
                    "calls": int(row[3]) if row[3] is not None else 0,
                    "puts": int(row[4]) if row[4] is not None else 0,
                    "strike_min": float(row[5]) if row[5] is not None else None,
                    "strike_max": float(row[6]) if row[6] is not None else None,
                    "underlying": row[7],
                }
                for row in cur.fetchall()
            ]

        return {
            "equity": {
                "total": equity_total,
                "rows": equity_rows,
            },
            "option": {
                "total": option_total,
                "chains": option_chains,
                "rows": option_rows,
            },
        }
    except Exception as exc:  # pragma: no cover - simple wrapper
        raise DBError(f"Failed to fetch contracts summary: {exc}") from exc
    finally:
        conn.close()

