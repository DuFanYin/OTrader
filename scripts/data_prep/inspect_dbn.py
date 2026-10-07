#!/usr/bin/env python3
"""Print a DBN file's metadata, symbol mappings and first records.

Usage: python inspect_dbn.py FILE.dbn [--rows 5] [--symbols 10]
"""

from __future__ import annotations

import argparse
import sys

import databento as db

from dbn_common import ns_to_et, top_of_book


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("file")
    ap.add_argument("--rows", type=int, default=5)
    ap.add_argument("--symbols", type=int, default=10)
    args = ap.parse_args(argv)

    store = db.DBNStore.from_file(args.file)
    meta = store.metadata
    print(f"dataset   {meta.dataset}")
    print(f"schema    {meta.schema}")
    print(f"start     {meta.start}")
    print(f"end       {meta.end}")
    print(f"stype_in  {getattr(meta, 'stype_in', '-')}")

    mappings = meta.mappings or {}
    id_to_raw: dict[int, str] = {}
    for raw, intervals in mappings.items():
        for iv in intervals:
            if iv.get("symbol") is not None:
                id_to_raw[int(iv["symbol"])] = raw
    print(f"\n{len(mappings):,} symbol mappings")
    for raw, intervals in list(mappings.items())[: args.symbols]:
        for iv in intervals:
            span = f"{iv.get('start_date')} .. {iv.get('end_date')}"
            print(f"  {raw!r:28} id={iv.get('symbol')}  {span}")

    print(f"\nfirst {args.rows} records")
    print(
        f"{'instrument_id':>14}  {'symbol':24}  {'ts_recv (ET)':26}  "
        f"{'bid_px':>10}  {'ask_px':>10}  {'bid_sz':>7}  {'ask_sz':>7}"
    )
    for i, rec in enumerate(store):
        if i >= args.rows:
            break
        iid = getattr(rec, "instrument_id", None)
        ts = getattr(rec, "ts_recv", None)
        bid, ask, bsz, asz = top_of_book(rec)
        when = ns_to_et(ts).isoformat() if ts else "-"
        print(
            f"{iid!s:>14}  {id_to_raw.get(iid, ''):24}  {when:26}  "
            f"{bid!s:>10}  {ask!s:>10}  {bsz!s:>7}  {asz!s:>7}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
