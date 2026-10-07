#!/usr/bin/env python3
"""Decompress every *.zst in a directory (Databento batch downloads): foo.dbn.zst -> foo.dbn.

Usage: python decompress_zst.py DIR   (searched recursively; the .zst files are kept)
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import zstandard as zstd


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("directory")
    args = ap.parse_args(argv)

    d = Path(args.directory)
    if not d.is_dir():
        print(f"not a directory: {d}", file=sys.stderr)
        return 1
    files = sorted(d.rglob("*.zst"))
    dctx = zstd.ZstdDecompressor()
    for src in files:
        dst = src.with_suffix("")
        print(f"{src.name} -> {dst.name}")
        with open(src, "rb") as f_in, open(dst, "wb") as f_out:
            dctx.copy_stream(f_in, f_out)
    print(f"decompressed {len(files)} file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
