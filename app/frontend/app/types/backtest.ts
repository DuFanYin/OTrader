export interface FileInfo {
  name: string;
  path: string;
  type: "dbn" | "parquet" | "segment";
  size_bytes: number;
  number_of_days?: number | null;
  file_count?: number | null;
  date_start?: string | null;
  date_end?: string | null;
}

export interface FileDetail {
  path: string;
  size_bytes?: number;
  number_of_days?: number | null;
  file_count?: number | null;
  date_start?: string | null;
  date_end?: string | null;
}

export interface TimestepMetric {
  timestep: number;
  timestamp: string;
  pnl: number;
  delta: number;
  theta: number;
  gamma: number;
}

export interface DailyResult {
  file: string;
  pnl: number;
  net_pnl?: number;
  fees: number;
  trades: number;
  timesteps: number;
  rows: number;
}

export interface ProgressInfo {
  completed?: number;
  total?: number;
  progress?: number;
}

export interface BacktestResult {
  strategy_name: string;
  portfolio_name: string;
  start_time: string;
  end_time: string;
  total_timesteps: number;
  processed_timesteps: number;
  total_frames?: number;
  total_rows?: number;
  final_pnl: number;
  net_pnl?: number;
  total_trades: number;
  max_delta: number;
  max_gamma: number;
  max_theta: number;
  daily_sharpe?: number;
  max_drawdown?: number;
  total_fees?: number;
  fee_rate?: number;
  slippage_bps?: number;
  risk_free_rate?: number;
  iv_price_mode?: string;
  num_days?: number;
  duration_seconds?: number;
  duration_ms?: number;
  progress_info?: ProgressInfo;
}

export interface BacktestResponse {
  status: "ok" | "error" | "cancelled";
  result?: BacktestResult;
  timestep_metrics?: TimestepMetric[];
  daily_results?: DailyResult[];
  chart_svg?: string;
  chart_image_base64?: string;
  errors?: string[];
  error?: string;
}

export interface StrategyOption {
  value: string;
  label: string;
}
