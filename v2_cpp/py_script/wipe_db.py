#!/usr/bin/env python3
"""
Wipe Otrader-related tables in PostgreSQL (contract_equity, contract_option, orders, trades).
Uses DATABASE_URL from .env (python-dotenv).

Usage:
  python wipe_db.py           # wipe all
  python wipe_db.py --dry-run # print SQL only, no execute
"""
from __future__ import annotations

import argparse
import os
import sys

try:
    from dotenv import load_dotenv
except ImportError:
    load_dotenv = None

try:
    import psycopg2
except ImportError:
    psycopg2 = None

# Tables used by Otrader/infra/db/engine_db_pg.cpp
TABLES = ("contract_option", "contract_equity", "orders", "trades")


def main() -> None:
    parser = argparse.ArgumentParser(description="Wipe Otrader DB tables (contract_*, orders, trades).")
    parser.add_argument("--dry-run", action="store_true", help="Print SQL only, do not execute")
    args = parser.parse_args()

    if load_dotenv is not None:
        load_dotenv()
    conninfo = os.environ.get("DATABASE_URL", "").strip()
    if not conninfo:
        print("DATABASE_URL not set. Create a .env with DATABASE_URL=postgresql://...", file=sys.stderr)
        sys.exit(1)

    if not psycopg2:
        print("psycopg2 is required: pip install psycopg2-binary", file=sys.stderr)
        sys.exit(1)

    if args.dry_run:
        for t in TABLES:
            print(f"TRUNCATE TABLE {t};")
        print("-- dry-run, not executed")
        return

    conn = psycopg2.connect(conninfo)
    try:
        with conn.cursor() as cur:
            for t in TABLES:
                cur.execute(f"TRUNCATE TABLE {t}")
                print(f"Truncated {t}")
        conn.commit()
        print("Done.")
    except Exception as e:
        conn.rollback()
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    finally:
        conn.close()


if __name__ == "__main__":
    main()
