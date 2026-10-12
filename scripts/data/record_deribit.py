"""Re-record the offline Deribit option-chain fixtures used as the last-resort fallback and in tests.

Usage:  uv run --project app/server python scripts/data/record_deribit.py [BTC ETH]
Stores the raw get_instruments / get_book_summary_by_currency responses, trimmed to
the fields normalize_deribit_chain() reads, plus the recording time.
"""

import json
import sys
from datetime import datetime, timezone
from pathlib import Path

import httpx

# The fallback chains the server reads (optra.services.market_data).
FIXTURES = Path(__file__).resolve().parents[2] / "app" / "server" / "optra" / "services" / "fixtures"

BASE = "https://www.deribit.com/api/v2/public/"
INSTRUMENT_FIELDS = ("instrument_name", "strike", "option_type", "expiration_timestamp")
SUMMARY_FIELDS = ("instrument_name", "bid_price", "ask_price", "mark_price", "mark_iv", "volume",
                  "open_interest", "underlying_price", "estimated_delivery_price", "interest_rate")


def record(currency: str) -> Path:
    with httpx.Client(timeout=20) as c:
        fetched_at = datetime.now(timezone.utc)
        inst = c.get(BASE + "get_instruments",
                     params={"currency": currency, "kind": "option", "expired": "false"}).json()["result"]
        summ = c.get(BASE + "get_book_summary_by_currency",
                     params={"currency": currency, "kind": "option"}).json()["result"]
    out = {
        "currency": currency,
        "fetched_at": fetched_at.isoformat(),
        "instruments": [{k: i.get(k) for k in INSTRUMENT_FIELDS} for i in inst],
        "summaries": [{k: s.get(k) for k in SUMMARY_FIELDS} for s in summ],
    }
    path = FIXTURES / f"deribit_{currency.lower()}_chain.json"
    path.write_text(json.dumps(out, separators=(",", ":")))
    print(f"{currency}: {len(inst)} instruments, {len(summ)} summaries -> {path.name}")
    return path


if __name__ == "__main__":
    for cur in (sys.argv[1:] or ["BTC", "ETH"]):
        record(cur.upper())
