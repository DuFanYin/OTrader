'use client';

import React, { useEffect, useMemo, useRef, useState } from "react";
import { api, getApiBase } from "@/lib/api";
import PageLayout from "@/app/components/PageLayout";
import TerminalSelect from "@/app/components/TerminalSelect";
import type {
  BacktestResponse,
  FileDetail,
  FileInfo,
  StrategyOption,
} from "@/app/types/backtest";

const labelClass =
  "w-24 shrink-0 text-right form-label whitespace-nowrap";
const inputClass =
  "w-40 form-input text-xs rounded-none";
const inputClassDate =
  "w-40 form-input text-xs rounded-none";

function MetricCard({
  label,
  children,
  valueClassName,
}: {
  label: string;
  children: React.ReactNode;
  valueClassName?: string;
}) {
  return (
    <div className="panel px-2 py-2">
      <p className="text-[10px] uppercase tracking-wide text-[color:var(--text-muted)] mb-1">
        {label}
      </p>
      <p
        className={`leading-none font-semibold numeric-12 ${valueClassName ?? "text-base text-[color:var(--text-primary)]"}`}
      >
        {children}
      </p>
    </div>
  );
}

function ToggleButtonGroup({
  value,
  onChange,
  options,
  disabled = false,
}: {
  value: "mid" | "bid" | "ask";
  onChange: (value: "mid" | "bid" | "ask") => void;
  options: Array<{ value: "mid" | "bid" | "ask"; label: string }>;
  disabled?: boolean;
}) {
  return (
    <div className="flex gap-1">
      {options.map((option) => (
        <button
          key={option.value}
          type="button"
          onClick={() => onChange(option.value)}
          disabled={disabled}
          className={`flex-1 h-7 px-2 text-xs uppercase tracking-wide border rounded-none transition-colors ${
            value === option.value
              ? "border-[color:var(--border-strong)] bg-[color:var(--surface-raised)] text-[color:var(--text-primary)]"
              : "border-[color:var(--border-subtle)] bg-[color:var(--surface-subtle)] text-[color:var(--text-soft)] hover:bg-[color:var(--surface-raised)] hover:text-[color:var(--text-primary)]"
          } disabled:opacity-50 disabled:cursor-not-allowed`}
        >
          {option.label}
        </button>
      ))}
    </div>
  );
}

export default function BacktestPage() {
  const [files, setFiles] = useState<FileInfo[]>([]);
  const [loading, setLoading] = useState(true);
  const [strategies, setStrategies] = useState<StrategyOption[]>([]);
  const [strategiesLoading, setStrategiesLoading] = useState(true);
  const [selectedSymbol, setSelectedSymbol] = useState<string>("");
  const [selectedDateStart, setSelectedDateStart] = useState<string>("");
  const [selectedDateEnd, setSelectedDateEnd] = useState<string>("");
  const [selectedStrategy, setSelectedStrategy] = useState<string>("");
  const [fileDetail, setFileDetail] = useState<FileDetail | null>(null);

  const [running, setRunning] = useState(false);
  const [backtestResult, setBacktestResult] = useState<BacktestResponse | null>(null);
  type StatusBarPhase = "running" | "finalising" | "completed";
  const [statusBarPhase, setStatusBarPhase] = useState<StatusBarPhase>("completed");
  const [feeRate, setFeeRate] = useState<number>(0.35);
  const [slippageBps, setSlippageBps] = useState<number>(5);
  const [riskFreeRate, setRiskFreeRate] = useState<number>(0.05);
  const [ivPriceMode, setIvPriceMode] = useState<"mid" | "bid" | "ask">("mid");

  const handleStopBacktest = async () => {
    try {
      const data = await api.post<{ status?: string }>("/api/backtest/cancel");
      if (data.status === "ok") {
        setStatusBarPhase("completed");
      }
    } catch (error) {
      console.error("Error cancelling backtest", error);
    } finally {
      setRunning(false);
    }
  };

  useEffect(() => {
    const fetchFiles = async () => {
      try {
        const data = await api.get<FileInfo[]>("/api/files");
        setFiles(Array.isArray(data) ? data : []);
      } catch (error) {
        console.error("Error fetching files:", error);
      } finally {
        setLoading(false);
      }
    };

    const fetchStrategies = async () => {
      try {
        const data = await api.get<{ status?: string; strategies?: StrategyOption[]; error?: string }>("/api/backtest/strategies");
        if (data.status === "ok" && data.strategies) {
          setStrategies(data.strategies);
        }
      } catch (error) {
        console.error("Error fetching strategies:", error);
      } finally {
        setStrategiesLoading(false);
      }
    };

    fetchFiles();
    fetchStrategies();
  }, []);

  const availableSymbols = useMemo(() => {
    const segments = files.filter((f) => f.type === "segment");
    return Array.from(
      new Set(segments.map((f) => f.path.split("/").pop() ?? "").filter(Boolean))
    ).sort();
  }, [files]);

  // Prefill first symbol when files are loaded
  useEffect(() => {
    if (loading || selectedSymbol !== "" || availableSymbols.length === 0) return;
    setSelectedSymbol(availableSymbols[0]);
  }, [loading, availableSymbols, selectedSymbol]);

  // Prefill first strategy when strategies are loaded
  useEffect(() => {
    if (strategiesLoading || selectedStrategy !== "" || strategies.length === 0) return;
    setSelectedStrategy(strategies[0].value);
  }, [strategiesLoading, strategies, selectedStrategy]);

  useEffect(() => {
    if (!selectedSymbol) {
      setFileDetail(null);
      setSelectedDateStart("");
      setSelectedDateEnd("");
      return;
    }
    const selected = files.find((f) => {
      if (f.type !== "segment") return false;
      const pathParts = f.path.split("/");
      const symbol = pathParts[pathParts.length - 1];
      return symbol === selectedSymbol;
    });
    if (!selected) {
      setFileDetail(null);
      setSelectedDateStart("");
      setSelectedDateEnd("");
      return;
    }
    setFileDetail({
      path: selected.path,
      size_bytes: selected.size_bytes,
      number_of_days: selected.number_of_days,
      file_count: selected.file_count,
      date_start: selected.date_start ?? null,
      date_end: selected.date_end ?? null,
    });
    if (selected.date_start && selected.date_end) {
      setSelectedDateStart(selected.date_start);
      setSelectedDateEnd(selected.date_end);
    } else {
      setSelectedDateStart("");
      setSelectedDateEnd("");
    }
  }, [selectedSymbol, files]);

  const handleRunBacktest = async () => {
    if (!selectedSymbol || !selectedStrategy) {
      alert("Please select both a symbol and a strategy");
      return;
    }
    if (!selectedDateStart || !selectedDateEnd) {
      alert("Please enter both start and end dates");
      return;
    }

    setRunning(true);
    setBacktestResult({ status: "ok", timestep_metrics: [] });
    setStatusBarPhase("running");

    try {
      const data = await api.post<BacktestResponse>("/api/run_backtest", {
        parquet: selectedSymbol,
        strategy: selectedStrategy,
        fee_rate: feeRate,
        slippage_bps: slippageBps,
        risk_free_rate: riskFreeRate,
        iv_price_mode: ivPriceMode,
        strategy_setting: {},
        start_date: selectedDateStart,
        end_date: selectedDateEnd,
      });

      if (data.status === "error") {
        setBacktestResult({
          status: "error",
          error: data.error || "Unknown error",
        });
        setStatusBarPhase("completed");
      } else if (data.status === "cancelled") {
        setBacktestResult({
          status: "cancelled",
          error: data.error || "Backtest cancelled by user",
        });
        setStatusBarPhase("completed");
      } else {
        setBacktestResult({
          status: "ok",
          result: data.result,
          timestep_metrics: data.timestep_metrics || [],
          daily_results: data.daily_results || [],
          chart_svg: data.chart_svg,
          chart_image_base64: data.chart_image_base64,
          errors: data.errors,
        });
        setStatusBarPhase("finalising");
      }
    } catch (error) {
      setBacktestResult({
        status: "error",
        error: `Network error: ${error instanceof Error ? error.message : "Unknown error"}`,
      });
      setStatusBarPhase("completed");
    } finally {
      setRunning(false);
    }
  };

  // After "finalising", show "completed" briefly then keep green
  const finalisingTimeoutRef = useRef<NodeJS.Timeout | null>(null);
  useEffect(() => {
    if (statusBarPhase !== "finalising") return;
    finalisingTimeoutRef.current = setTimeout(() => setStatusBarPhase("completed"), 800);
    return () => {
      if (finalisingTimeoutRef.current) {
        clearTimeout(finalisingTimeoutRef.current);
        finalisingTimeoutRef.current = null;
      }
    };
  }, [statusBarPhase]);

  // On page refresh or navigate away: cancel any running backtest so backend clears the process.
  // sendBeacon is used on beforeunload so the request is sent even when the page is unloading.
  const cancelBacktestUrl = `${getApiBase()}/api/backtest/cancel`;
  useEffect(() => {
    const handleBeforeUnload = () => {
      try {
        navigator.sendBeacon(cancelBacktestUrl, new Blob([], { type: "application/json" }));
      } catch {
        // ignore
      }
    };

    window.addEventListener("beforeunload", handleBeforeUnload);

    return () => {
      window.removeEventListener("beforeunload", handleBeforeUnload);
      try {
        fetch(cancelBacktestUrl, { method: "POST", keepalive: true }).catch(() => {});
      } catch {
        // ignore
      }
    };
  }, [cancelBacktestUrl]);

  const formatFileSize = (bytes: number) => {
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(2)} KB`;
    return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
  };

  return (
    <PageLayout title="Option Strategy Backtester">
      <React.Fragment>
        {/* 主内容区域：左列配置固定，右列结果单独滚动 */}
        <div className="flex-1 min-h-0 overflow-hidden">
          <div className="h-full flex flex-col lg:flex-row gap-3 min-h-0">
            {/* 左侧：所有配置表单（约 30% 宽度） */}
            <div className="w-full lg:w-[30%] flex-shrink-0">
              <div className="h-full border border-[color:var(--border-subtle)] px-3 py-2 space-y-2">
                <h2 className="text-xs uppercase tracking-[0.12em] text-[color:var(--text-muted)]">
                  Backtest Configuration
                </h2>

                <div className="space-y-3">
                  <div className="space-y-2">
                    <div className="flex items-center gap-2">
                      <label className={labelClass}>Symbol</label>
                      <TerminalSelect
                        value={selectedSymbol}
                        onValueChange={setSelectedSymbol}
                        className={inputClass}
                        disabled={loading || running || availableSymbols.length === 0}
                        placeholder={
                          loading
                            ? "Loading symbols..."
                            : availableSymbols.length === 0
                              ? "No symbols found"
                              : "Select a symbol..."
                        }
                        options={availableSymbols.map((symbol) => ({ value: symbol, label: symbol }))}
                      />
                    </div>

                    <div>
                      <label className={labelClass}>Available Duration</label>
                      <p className="text-xs numeric-12 text-[color:var(--state-success)]">
                        {fileDetail?.date_start && fileDetail?.date_end
                          ? `${fileDetail.date_start} ~ ${fileDetail.date_end}`
                          : "-"}
                      </p>
                      {fileDetail?.number_of_days != null && fileDetail?.size_bytes != null ? (
                        <p className="text-xs numeric-12 text-[color:var(--state-success)] mt-1">
                          {fileDetail.number_of_days} days, {formatFileSize(fileDetail.size_bytes)}
                        </p>
                      ) : (
                        <p className="text-xs numeric-12 text-[color:var(--text-muted)] mt-1">-</p>
                      )}
                    </div>

                    <div className="space-y-2">
                      <div className="flex items-center gap-2">
                        <label className={labelClass}>Start Date</label>
                        <input
                          type="date"
                          value={selectedDateStart}
                          onChange={(e) => setSelectedDateStart(e.target.value)}
                          className={inputClassDate}
                          disabled={running || !selectedSymbol}
                        />
                      </div>
                      <div className="flex items-center gap-2">
                        <label className={labelClass}>End Date</label>
                        <input
                          type="date"
                          value={selectedDateEnd}
                          onChange={(e) => setSelectedDateEnd(e.target.value)}
                          className={inputClassDate}
                          disabled={running || !selectedSymbol}
                        />
                      </div>
                    </div>

                    <div className="flex items-center gap-2">
                      <label className={labelClass}>Strategy</label>
                      <TerminalSelect
                        value={selectedStrategy}
                        onValueChange={setSelectedStrategy}
                        className={inputClass}
                        disabled={running || strategiesLoading}
                        placeholder={
                          strategiesLoading
                            ? "Loading strategies..."
                            : strategies.length === 0
                              ? "No strategies found"
                              : "Select a strategy..."
                        }
                        options={strategies.map((s) => ({ value: s.value, label: s.label }))}
                      />
                    </div>
                  </div>

                  <div className="space-y-2">
                    <div className="flex items-center gap-2">
                      <label className={labelClass}>Fee Rate</label>
                      <input
                        value={feeRate}
                        onChange={(e) => setFeeRate(Number(e.target.value) || 0)}
                        type="number"
                        step="0.0001"
                        min="0"
                        className={inputClass}
                        disabled={running}
                      />
                    </div>
                    <div className="flex items-center gap-2">
                      <label className={labelClass}>Slippage (bps)</label>
                      <input
                        value={slippageBps}
                        onChange={(e) => setSlippageBps(Number(e.target.value) || 0)}
                        type="number"
                        step="1"
                        min="0"
                        className={inputClass}
                        disabled={running}
                      />
                    </div>
                    <div className="flex items-center gap-2">
                      <label className={labelClass}>Risk Free Rate</label>
                      <input
                        value={riskFreeRate}
                        onChange={(e) => setRiskFreeRate(Number(e.target.value) || 0)}
                        type="number"
                        step="0.0001"
                        className={inputClass}
                        disabled={running}
                      />
                    </div>
                    <div className="flex items-center gap-2">
                      <label className={labelClass}>IV Price Mode</label>
                      <div className="flex-1">
                        <ToggleButtonGroup
                          value={ivPriceMode}
                          onChange={setIvPriceMode}
                          options={[
                            { value: "mid", label: "Mid" },
                            { value: "bid", label: "Bid" },
                            { value: "ask", label: "Ask" },
                          ]}
                          disabled={running}
                        />
                      </div>
                    </div>
                  </div>

                  <div className="flex items-center justify-end pt-1 gap-2">
                    <button
                      onClick={handleRunBacktest}
                      disabled={
                        !selectedSymbol ||
                        !selectedDateStart ||
                        !selectedDateEnd ||
                        !selectedStrategy ||
                        running
                      }
                      className="h-7 px-4 btn btn-primary disabled:text-[color:var(--text-muted)] disabled:border-[color:var(--border-subtle)] disabled:cursor-not-allowed rounded-none"
                    >
                      {running ? "Running..." : "Start Backtest"}
                    </button>
                    <button
                      type="button"
                      onClick={handleStopBacktest}
                      disabled={!running}
                      className="h-7 px-3 btn btn-danger disabled:opacity-40 disabled:cursor-not-allowed"
                    >
                      Stop
                    </button>
                  </div>
                </div>
              </div>
            </div>

            {/* 右侧：结果区域（状态条、错误、指标、图表、日度结果），在自身内部滚动 */}
            <div className="mt-3 lg:mt-0 flex-1 min-h-0 flex flex-col space-y-3 overflow-auto">
              {/* Status bar between config and chart when running/finalising */}
              {(statusBarPhase === "running" || statusBarPhase === "finalising") && (
                <div className="w-full py-2 px-3 text-center text-xs uppercase tracking-wide font-medium bg-[color:var(--surface-subtle)] text-[color:var(--text-muted)] border border-[color:var(--border-subtle)]">
                  {statusBarPhase === "running" ? "Running" : "Finalising"}
                </div>
              )}

              {backtestResult?.status === "error" && (
                <div className="border border-[color:var(--state-error)] bg-[color:var(--surface-subtle)] px-3 py-2">
                  <p className="text-xs text-[color:var(--state-error)]">
                    <span className="font-medium">Error:</span> {backtestResult.error}
                  </p>
                </div>
              )}

              {backtestResult?.status === "cancelled" && (
                <div className="border border-[color:var(--border-subtle)] bg-[color:var(--surface-subtle)] px-3 py-2">
                  <p className="text-xs text-[color:var(--text-muted)]">
                    Backtest cancelled by user.
                  </p>
                </div>
              )}

              {(() => {
                const r = backtestResult?.result;
                const hasResult = backtestResult?.status === "ok" && !!r;
                const showValues = hasResult && statusBarPhase === "completed";
                const netPnl =
                  r != null
                    ? r.net_pnl ?? r.final_pnl - (r.total_fees ?? 0)
                    : 0;

                const formatOrDash = (value: string | number | null | undefined) =>
                  showValues && value != null && value !== ""
                    ? String(value)
                    : "—";

                return (
                  <>
                    <div className="grid grid-cols-5 gap-2">
                      <MetricCard
                        label="Final PnL"
                        valueClassName={`text-lg ${
                          showValues && r && r.final_pnl >= 0
                            ? "text-[color:var(--state-success)]"
                            : showValues
                              ? "text-[color:var(--state-error)]"
                              : "text-[color:var(--text-soft)]"
                        }`}
                      >
                        {showValues && r
                          ? `$${r.final_pnl.toFixed(2)}`
                          : "—"}
                      </MetricCard>

                      <MetricCard
                        label="Net PnL"
                        valueClassName={`text-lg ${
                          showValues && r && netPnl >= 0
                            ? "text-[color:var(--state-success)]"
                            : showValues
                              ? "text-[color:var(--state-error)]"
                              : "text-[color:var(--text-soft)]"
                        }`}
                      >
                        {showValues && r
                          ? `$${netPnl.toFixed(2)}`
                          : "—"}
                      </MetricCard>

                      <MetricCard label="Sharpe (Daily)">
                        {showValues && r && r.daily_sharpe != null
                          ? Number(r.daily_sharpe).toFixed(3)
                          : "—"}
                      </MetricCard>

                      <MetricCard label="Max DD">
                        {showValues && r && r.max_drawdown != null
                          ? Number(r.max_drawdown).toFixed(2)
                          : "—"}
                      </MetricCard>

                      <MetricCard label="Total Trades">
                        {showValues && r
                          ? formatOrDash(r.total_trades)
                          : "—"}
                      </MetricCard>

                      <MetricCard label="Total Fees">
                        {showValues && r
                          ? `$${Number(r.total_fees ?? 0).toFixed(2)}`
                          : "—"}
                      </MetricCard>

                      <MetricCard label="Days">
                        {showValues && r && r.num_days != null && r.num_days > 1
                          ? r.num_days
                          : "—"}
                      </MetricCard>

                      <MetricCard label="Duration">
                        {showValues && r && r.duration_seconds != null ? (
                          r.duration_seconds < 1 ? (
                            `${Math.round(r.duration_ms ?? 0)}ms`
                          ) : (
                            `${r.duration_seconds.toFixed(2)}s`
                          )
                        ) : (
                          "—"
                        )}
                      </MetricCard>

                      <MetricCard label="Max Delta">
                        {showValues && r
                          ? r.max_delta.toFixed(4)
                          : "—"}
                      </MetricCard>

                      <MetricCard label="Max Gamma">
                        {showValues && r
                          ? r.max_gamma.toFixed(4)
                          : "—"}
                      </MetricCard>

                      <MetricCard label="Max Theta">
                        {showValues && r
                          ? r.max_theta.toFixed(4)
                          : "—"}
                      </MetricCard>

                      <MetricCard label="Frames">
                        {showValues && r
                          ? formatOrDash(r.total_frames ?? r.processed_timesteps)
                          : "—"}
                      </MetricCard>

                      <MetricCard label="Rows">
                        {showValues && r
                          ? (r.total_rows ?? 0).toLocaleString()
                          : "—"}
                      </MetricCard>
                    </div>

                    {/* Status bar below metrics when completed */}
                    {hasResult && statusBarPhase === "completed" && (
                      <div className="w-full py-2 px-3 mt-2 text-center text-xs uppercase tracking-wide font-medium border border-[color:var(--border-subtle)] text-[color:var(--text-muted)]">
                        Completed
                      </div>
                    )}
                  </>
                );
              })()}

              {(running || backtestResult?.chart_svg || backtestResult?.chart_image_base64) && (
                <div className="border border-[color:var(--border-subtle)] px-2 py-2">
                  <div className="relative min-h-[320px] flex flex-col">
                    {!backtestResult?.chart_svg && !backtestResult?.chart_image_base64 ? (
                      <div className="absolute inset-0 flex items-center justify-center">
                        <p className="text-sm text-[color:var(--text-muted)]">Waiting for chart...</p>
                      </div>
                    ) : (
                      <>
                        <h3 className="text-xs uppercase tracking-[0.12em] mb-1 text-[color:var(--text-muted)]">
                          PnL / Delta / Theta / Gamma (backend)
                        </h3>
                        {/* eslint-disable-next-line @next/next/no-img-element -- inline chart from backend (SVG or base64) */}
                        <img
                          src={
                            backtestResult?.chart_svg
                              ? `data:image/svg+xml;charset=utf-8,${encodeURIComponent(backtestResult.chart_svg)}`
                              : `data:image/png;base64,${backtestResult?.chart_image_base64 ?? ""}`
                          }
                          alt="Backtest chart"
                          className="w-full max-w-full h-auto border border-[color:var(--border-subtle)]"
                        />
                      </>
                    )}
                  </div>
                </div>
              )}

              {backtestResult?.daily_results && backtestResult.daily_results.length > 0 && (
                <div className="panel px-3 py-2">
                  <h3 className="text-xs uppercase tracking-[0.12em] mb-2 text-[color:var(--text-muted)]">
                    Daily Results ({backtestResult.daily_results.length} days)
                  </h3>
                  <div className="overflow-x-auto">
                    <table className="table-terminal">
                      <thead>
                        <tr>
                          <th className="text-left">Date</th>
                          <th className="text-right">PnL</th>
                          <th className="text-right">Net PnL</th>
                          <th className="text-right">Fees</th>
                          <th className="text-right">Trades</th>
                          <th className="text-right">Timesteps</th>
                          <th className="text-right">Rows</th>
                        </tr>
                      </thead>
                      <tbody>
                        {backtestResult.daily_results.map((daily, idx) => {
                          const filePath = daily.file;
                          const fileName = filePath.split('/').pop() || '';
                          const dateStr = fileName.replace('.parquet', '');
                          let displayDate = dateStr;
                          if (dateStr.length === 8 && /^\d{8}$/.test(dateStr)) {
                            displayDate = `${dateStr.slice(0, 4)}-${dateStr.slice(4, 6)}-${dateStr.slice(6, 8)}`;
                          }
                          const netPnl = daily.net_pnl ?? (daily.pnl - daily.fees);
                          return (
                            <tr key={idx} className="table-row-hover">
                              <td className="numeric-12 text-left text-[color:var(--text-primary)]">
                                {displayDate}
                              </td>
                              <td className={`numeric-12 text-right ${daily.pnl >= 0 ? 'text-[color:var(--state-success)]' : 'text-[color:var(--state-error)]'}`}>
                                ${daily.pnl.toFixed(2)}
                              </td>
                              <td className={`numeric-12 text-right ${netPnl >= 0 ? 'text-[color:var(--state-success)]' : 'text-[color:var(--state-error)]'}`}>
                                ${netPnl.toFixed(2)}
                              </td>
                              <td className="numeric-12 text-right text-[color:var(--text-soft)]">
                                ${daily.fees.toFixed(2)}
                              </td>
                              <td className="numeric-12 text-right text-[color:var(--text-primary)]">
                                {daily.trades}
                              </td>
                              <td className="numeric-12 text-right text-[color:var(--text-soft)]">
                                {daily.timesteps.toLocaleString()}
                              </td>
                              <td className="numeric-12 text-right text-[color:var(--text-soft)]">
                                {daily.rows.toLocaleString()}
                              </td>
                            </tr>
                          );
                        })}
                      </tbody>
                    </table>
                  </div>
                </div>
              )}
            </div>
          </div>
        </div>
      </React.Fragment>
    </PageLayout>
  );
}
