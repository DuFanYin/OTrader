import type { FileInfo } from "./backtest";

export interface EquitySummaryRow {
  symbol: string;
  exchange: string;
  product: string;
  size: number | null;
  pricetick: number | null;
  gateway_name: string;
}

export interface OptionSummaryRow {
  chain: string;
  dte: number | null;
  calls: number;
  puts: number;
  strike_min: number | null;
  strike_max: number | null;
  underlying: string | null;
}

/** Reuse backtest file list type for /api/files. */
export type BacktestFile = FileInfo;
