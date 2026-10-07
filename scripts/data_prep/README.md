# Data prep: Databento DBN → backtest Parquet

Turns Databento downloads into the per-day Parquet files the backtester reads (schema and layout: [data/README.md](../../data/README.md)). This is the pipeline that produced the `data/SPXW` samples: SPXW option CBBO-1m quotes, with ES futures BBO-1m as the underlying.

```bash
python3 -m venv .venv && .venv/bin/pip install -r scripts/data_prep/requirements.txt
cd scripts/data_prep
```

## Pipeline

```bash
# 0. Databento batch downloads arrive zstd-compressed
./decompress_zst.py ~/downloads/OPRA-20250301-XXXX

# 1. Underlying: ES continuous BBO-1m -> one regular-session (09:30-16:00 ET) Parquet
./es_to_parquet.py ~/downloads/es_mini -o ~/downloads/es_rth.parquet --start 2025-03-01

# 2. Options: SPXW CBBO-1m DBN -> data/SPXW/SPXW-YYYY-MM/YYYYMMDD.parquet
./options_to_parquet.py ~/downloads/OPRA-20250301-XXXX \
    --underlying ~/downloads/es_rth.parquet --out ../../data --symbol SPXW

# 3. Check the result
./inspect_parquet.py ../../data/SPXW/SPXW-2025-03 --head 0
```

| Script | Does |
|---|---|
| `decompress_zst.py DIR` | `*.zst` → same name without `.zst` (originals kept) |
| `es_to_parquet.py INPUT... -o OUT` | Underlying quotes, regular session only, `--start/--end` day filter, duplicate timestamps keep the first file's row |
| `options_to_parquet.py INPUT... --underlying U --out ROOT` | Option quotes joined to the underlying at the same timestamp, split into one file per Eastern-time trading day, written in the exact backtest schema. Runs files in parallel (`--workers`); refuses to replace existing days without `--overwrite` |
| `inspect_dbn.py FILE` | DBN metadata, symbol mappings, first records |
| `inspect_parquet.py PATH...` | Row / timestamp / day counts and whether each file matches the backtest schema |

`options_to_parquet.py` fails a file — and writes nothing for it — when an option timestamp has no underlying row, or a symbol is not an OCC body the loader can parse. Other files keep converting; the failures are listed at the end and the exit code is 1.

Run the tests with `python test_data_prep.py`. The round-trip test rebuilds `data/SPXW/SPXW-2025-08/20250804.parquet` from its own option and underlying columns and requires an identical file and, if `Otrader/build/entry_backtest` exists, an identical backtest.

## Things to know about the data

- **Symbols are stored without the root.** Databento's `SPXW  250804C05000000` becomes `250804C05000000`; `parse_occ_symbol` reads expiry / right / strike from the first character. The root comes from the folder name.
- **Timestamps** are written as `timestamp[us, America/New_York]`. Arrow stores the UTC instant, so the zone only affects display and the trading-day split.
- **`symbol` is written as Arrow `string`.** polars defaults to `large_string`; older loaders silently dropped every option for that type (backtest "ok", no trades). The loader now accepts both, and these scripts write through pyarrow with an explicit schema.
- **The underlying is ES futures, not the SPX index.** It is the realistic hedge instrument, but its price is the forward to the ES contract's expiry. The engine feeds `underlying_*` mid straight into IV (Black, undiscounted — i.e. as the option expiry's forward) and into Greeks (as spot, with `--risk-free-rate`). The gap is the carry between the option expiry and the ES expiry: up to about a quarter of a year of (r − dividend yield), tens of index points. Treat absolute IV / delta levels with that in mind; a put-call-parity forward per expiry would remove it but needs an engine change, not a data change.
- **`ES.v.0` is a volume-based continuous contract.** On a roll day (e.g. 2025-03-19, March → June) the underlying level jumps by the calendar spread between one minute and the next.
