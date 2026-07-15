'use client';

import { useEffect, useState } from 'react';
import { api } from '@/lib/api';
import PageLayout from '@/app/components/PageLayout';
import type { BacktestFile, EquitySummaryRow, OptionSummaryRow } from '@/app/types/database';

export default function DatabasePage() {
  const [equityTotal, setEquityTotal] = useState<number>(0);
  const [equityRows, setEquityRows] = useState<EquitySummaryRow[]>([]);
  const [optionTotal, setOptionTotal] = useState<number>(0);
  const [optionChains, setOptionChains] = useState<number>(0);
  const [optionRows, setOptionRows] = useState<OptionSummaryRow[]>([]);
  const [contractsError, setContractsError] = useState<string | null>(null);

  const [files, setFiles] = useState<BacktestFile[]>([]);
  const [filesError, setFilesError] = useState<string | null>(null);

  useEffect(() => {
    const fetchPortfolios = async () => {
      try {
        await api.get<{ portfolios?: string[] }>('/api/data/portfolios');
      } catch {
        // ignore portfolio errors on this summary page
      }
    };

    const fetchContracts = async () => {
      try {
        const data = await api.get<{
          equity?: { total?: number; rows?: EquitySummaryRow[] };
          option?: { total?: number; chains?: number; rows?: OptionSummaryRow[] };
        }>('/api/database/contracts');
        setEquityTotal(data.equity?.total ?? 0);
        setEquityRows(data.equity?.rows ?? []);
        setOptionTotal(data.option?.total ?? 0);
        setOptionChains(data.option?.chains ?? 0);
        setOptionRows(data.option?.rows ?? []);
        setContractsError(null);
      } catch (e) {
        setContractsError(e instanceof Error ? e.message : 'Failed to load contracts');
        setEquityRows([]);
        setOptionRows([]);
      }
    };

    const fetchFiles = async () => {
      try {
        const data = await api.get<BacktestFile[]>('/api/files');
        setFiles(Array.isArray(data) ? data : []);
        setFilesError(null);
      } catch (e) {
        setFilesError(e instanceof Error ? e.message : 'Failed to load backtest files');
        setFiles([]);
      }
    };

    void fetchPortfolios();
    void fetchContracts();
    void fetchFiles();
  }, []);

  return (
    <PageLayout title="Database">
        <div className="grid grid-cols-1 md:grid-cols-2 gap-3 flex-1 min-h-0">
          {/* Left column: contracts from DB */}
          <div className="flex flex-col space-y-3">
            <section className="panel p-3 space-y-2">
              <div className="flex items-center justify-between">
                <p className="text-xs uppercase tracking-wide text-[color:var(--text-muted)]">
                  Contracts (Equity)
                </p>
                <span className="numeric-11 text-[color:var(--text-muted)]">
                  Total: {equityTotal.toLocaleString()}
                </span>
              </div>
              {contractsError ? (
                <p className="text-xs text-[color:var(--state-error)]">{contractsError}</p>
              ) : (
                <div className="overflow-auto max-h-64 border border-[color:var(--border-subtle)] bg-transparent">
                  <table className="table-terminal">
                    <thead>
                      <tr>
                        <th className="text-left">Symbol</th>
                        <th className="text-left">Exch</th>
                        <th className="text-left">Prod</th>
                        <th className="text-right">Size</th>
                        <th className="text-right">Tick</th>
                      </tr>
                    </thead>
                    <tbody>
                      {equityRows.map((row) => (
                        <tr key={row.symbol} className="table-row-hover">
                          <td className="numeric-12 text-left text-[color:var(--text-primary)]">
                            {row.symbol}
                          </td>
                          <td className="text-[color:var(--text-soft)]">{row.exchange}</td>
                          <td className="text-[color:var(--text-soft)]">{row.product}</td>
                          <td className="numeric-12 text-right text-[color:var(--text-primary)]">
                            {row.size != null ? row.size.toFixed(0) : '-'}
                          </td>
                          <td className="numeric-12 text-right text-[color:var(--text-primary)]">
                            {row.pricetick != null ? row.pricetick.toFixed(4) : '-'}
                          </td>
                        </tr>
                      ))}
                      {equityRows.length === 0 && (
                        <tr>
                          <td
                            colSpan={5}
                            className="py-3 px-2 text-center text-[color:var(--text-muted)]"
                          >
                            No equity contracts.
                          </td>
                        </tr>
                      )}
                    </tbody>
                  </table>
                </div>
              )}
            </section>

            <section className="panel p-3 space-y-2">
              <div className="flex items-center justify-between">
                <p className="text-xs uppercase tracking-wide text-[color:var(--text-muted)]">
                  Contracts (Option)
                </p>
                <div className="flex items-center gap-3 text-[11px] text-[color:var(--text-muted)] font-mono tabular-nums">
                  <span>Chains: {optionChains.toLocaleString()}</span>
                  <span>Options: {optionTotal.toLocaleString()}</span>
                </div>
              </div>
              {contractsError ? (
                <p className="text-xs text-[color:var(--state-error)]">{contractsError}</p>
              ) : (
                <div className="overflow-auto max-h-72 border border-[color:var(--border-subtle)] bg-transparent">
                  <table className="table-terminal">
                    <thead>
                      <tr>
                        <th className="text-left">Chain</th>
                        <th className="text-right">DTE</th>
                        <th className="text-right">Calls</th>
                        <th className="text-right">Puts</th>
                        <th className="text-right">Strike Min</th>
                        <th className="text-right">Strike Max</th>
                        <th className="text-left">Underlying</th>
                      </tr>
                    </thead>
                    <tbody>
                      {optionRows.map((row) => (
                        <tr key={row.chain} className="table-row-hover">
                          <td className="numeric-12 text-left text-[color:var(--text-primary)]">
                            {row.chain}
                          </td>
                          <td className="numeric-12 text-right text-[color:var(--text-primary)]">
                            {row.dte != null ? row.dte : '-'}
                          </td>
                          <td className="numeric-12 text-right text-[color:var(--text-primary)]">
                            {row.calls}
                          </td>
                          <td className="numeric-12 text-right text-[color:var(--text-primary)]">
                            {row.puts}
                          </td>
                          <td className="numeric-12 text-right text-[color:var(--text-primary)]">
                            {row.strike_min != null ? row.strike_min.toFixed(2) : '-'}
                          </td>
                          <td className="numeric-12 text-right text-[color:var(--text-primary)]">
                            {row.strike_max != null ? row.strike_max.toFixed(2) : '-'}
                          </td>
                          <td className="text-[color:var(--text-soft)]">{row.underlying ?? '-'}</td>
                        </tr>
                      ))}
                      {optionRows.length === 0 && (
                        <tr>
                          <td
                            colSpan={6}
                            className="py-3 px-2 text-center text-[color:var(--text-muted)]"
                          >
                            No option contracts.
                          </td>
                        </tr>
                      )}
                    </tbody>
                  </table>
                </div>
              )}
            </section>
          </div>

          {/* Right column: backtest files */}
          <div className="space-y-3">
            <section className="panel p-3 space-y-3">
              <p className="text-xs uppercase tracking-wide text-[color:var(--text-muted)]">
                Backtest Files
              </p>
              {filesError ? (
                <p className="text-xs text-[color:var(--state-error)]">{filesError}</p>
              ) : (
                <div className="overflow-auto max-h-[420px] border border-[color:var(--border-subtle)] bg-transparent">
                  <table className="table-terminal">
                    <thead>
                      <tr>
                        <th className="text-left">Name</th>
                        <th className="text-left">Type</th>
                        <th className="text-right">Days</th>
                        <th className="text-left">Start</th>
                        <th className="text-left">End</th>
                        <th className="text-right">Size (MB)</th>
                      </tr>
                    </thead>
                    <tbody>
                      {files.map((f) => (
                        <tr key={f.path} className="table-row-hover">
                          <td className="numeric-12 text-left text-[color:var(--text-primary)]">
                            {f.name}
                          </td>
                          <td className="text-[color:var(--text-soft)]">{f.type}</td>
                          <td className="numeric-12 text-right text-[color:var(--text-primary)]">
                            {f.number_of_days != null ? f.number_of_days : '-'}
                          </td>
                          <td className="numeric-11 text-[color:var(--text-soft)]">
                            {f.date_start ?? '-'}
                          </td>
                          <td className="numeric-11 text-[color:var(--text-soft)]">
                            {f.date_end ?? '-'}
                          </td>
                          <td className="numeric-12 text-right text-[color:var(--text-primary)]">
                            {(f.size_bytes / (1024 * 1024)).toFixed(2)}
                          </td>
                        </tr>
                      ))}
                      {files.length === 0 && !filesError && (
                        <tr>
                          <td
                            colSpan={6}
                            className="py-3 px-2 text-center text-[color:var(--text-muted)]"
                          >
                            No backtest files.
                          </td>
                        </tr>
                      )}
                    </tbody>
                  </table>
                </div>
              )}
            </section>
          </div>
        </div>
    </PageLayout>
  );
}

