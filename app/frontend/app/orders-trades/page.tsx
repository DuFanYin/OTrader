'use client';

import React, { useCallback, useEffect, useState } from 'react';
import { api } from '@/lib/api';
import PageLayout from '@/app/components/PageLayout';
import type { DbRecord, RecordTypeFilter } from '@/app/types/orders-trades';
import TerminalSelect from '@/app/components/TerminalSelect';

export default function OrdersTradesPage() {
  const [refreshing, setRefreshing] = useState(false);
  const [records, setRecords] = useState<DbRecord[]>([]);
  const [strategies, setStrategies] = useState<string[]>([]);
  const [selectedStrategy, setSelectedStrategy] = useState<string>('');
  const [recordType, setRecordType] = useState<RecordTypeFilter>('all');
  const [error, setError] = useState<string | null>(null);
  const [limit, setLimit] = useState<number>(100);

  const fetchData = useCallback(async () => {
    setRefreshing(true);
    setError(null);
    try {
      const effectiveLimit = Number.isFinite(limit) && limit > 0 ? Math.min(limit, 5000) : 100;
      const data = await api.get<{
        strategies?: string[];
        records?: DbRecord[];
        error?: string;
      }>(`/api/orders-trades/db?limit=${encodeURIComponent(effectiveLimit)}`);
      if (data.error) {
        setError(data.error);
        setRecords([]);
        return;
      }
      setStrategies(data.strategies ?? []);
      setRecords(data.records ?? []);
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Unknown error');
      setRecords([]);
    } finally {
      setRefreshing(false);
    }
  }, [limit]);

  useEffect(() => {
    void fetchData();
  }, [fetchData]);

  const filteredRecords = records.filter((r) => {
    const matchType =
      recordType === 'all' ||
      (recordType === 'order' && r.record_type === 'Order') ||
      (recordType === 'trade' && r.record_type === 'Trade');
    const matchStrategy = !selectedStrategy || r.strategy_name === selectedStrategy;
    return matchType && matchStrategy;
  });

  return (
    <PageLayout title="Orders & Trades">
      <React.Fragment>
        <div className="panel p-4 flex flex-col min-h-0 space-y-3 flex-1">
          <div className="flex flex-wrap items-center justify-between gap-3">
            <div className="flex flex-wrap items-center gap-3 text-xs">
              <div className="flex items-center gap-2">
                <span className="form-label">
                  Type
                </span>
                <div className="inline-flex border border-[color:var(--border-subtle)]">
                  <button
                    type="button"
                    onClick={() => setRecordType('all')}
                    className={`px-2 h-7 text-[11px] uppercase tracking-wide ${
                      recordType === 'all'
                        ? 'bg-[color:var(--surface-raised)] text-[color:var(--text-primary)]'
                        : 'bg-[color:var(--surface-subtle)] text-[color:var(--text-soft)] hover:text-[color:var(--text-primary)]'
                    }`}
                  >
                    All
                  </button>
                  <button
                    type="button"
                    onClick={() => setRecordType('order')}
                    className={`px-2 h-7 text-[11px] uppercase tracking-wide border-l border-[color:var(--border-subtle)] ${
                      recordType === 'order'
                        ? 'bg-[color:var(--surface-raised)] text-[color:var(--text-primary)]'
                        : 'bg-[color:var(--surface-subtle)] text-[color:var(--text-soft)] hover:text-[color:var(--text-primary)]'
                    }`}
                  >
                    Orders
                  </button>
                  <button
                    type="button"
                    onClick={() => setRecordType('trade')}
                    className={`px-2 h-7 text-[11px] uppercase tracking-wide border-l border-[color:var(--border-subtle)] ${
                      recordType === 'trade'
                        ? 'bg-[color:var(--surface-raised)] text-[color:var(--text-primary)]'
                        : 'bg-[color:var(--surface-subtle)] text-[color:var(--text-soft)] hover:text-[color:var(--text-primary)]'
                    }`}
                  >
                    Trades
                  </button>
                </div>
              </div>
              <div className="flex items-center gap-2">
                <span className="form-label">Strategy</span>
                <TerminalSelect
                  value={selectedStrategy}
                  onValueChange={setSelectedStrategy}
                  placeholder="All strategies"
                  className="min-w-[160px] text-[11px]"
                  options={strategies.map((s) => ({ value: s, label: s }))}
                />
              </div>
            </div>
            <div className="flex items-center gap-2">
              <span className="form-label">
                Rows
              </span>
              <input
                type="number"
                min={1}
                max={5000}
                value={limit}
                onChange={(e) => {
                  const v = Number(e.target.value);
                  setLimit(Number.isFinite(v) && v > 0 ? v : 100);
                }}
                className="h-7 w-20 form-input text-[11px] numeric-11"
              />
              {error && (
                <span className="text-[11px] text-[color:var(--state-error)] max-w-xs truncate">
                  {error}
                </span>
              )}
              <button
                type="button"
                onClick={fetchData}
                className="h-7 px-4 btn btn-primary disabled:opacity-50"
                disabled={refreshing}
              >
                {refreshing ? 'Refreshing...' : 'Refresh'}
              </button>
            </div>
          </div>

          <div className="flex-1 overflow-auto min-h-[300px]">
            <div className="panel bg-transparent overflow-x-auto">
              <table className="table-terminal">
                <thead>
                  <tr>
                    <th className="text-left">
                      Time
                    </th>
                    <th className="text-left">
                      Type
                    </th>
                    <th className="text-left">
                      Strategy
                    </th>
                    <th className="text-left">
                      Symbol
                    </th>
                    <th className="text-left">
                      Side
                    </th>
                    <th className="text-right">
                      Price
                    </th>
                    <th className="text-right">
                      Volume
                    </th>
                    <th className="text-left">
                      Status
                    </th>
                  </tr>
                </thead>
                <tbody>
                  {filteredRecords.length === 0 ? (
                    <tr>
                      <td
                        colSpan={8}
                        className="py-8 px-2 text-center text-[color:var(--text-muted)]"
                      >
                        {refreshing
                          ? 'Loading orders & trades from database...'
                          : 'No records.'}
                      </td>
                    </tr>
                  ) : (
                    filteredRecords.map((r, idx) => (
                      <tr
                        key={`${r.record_type}-${r.id}-${idx}`}
                        className="table-row-hover"
                      >
                        <td className="numeric-12 text-left text-[color:var(--text-primary)]">
                          {r.timestamp}
                        </td>
                        <td className="text-[color:var(--text-primary)]">{r.record_type}</td>
                        <td className="text-[color:var(--text-primary)]">{r.strategy_name}</td>
                        <td className="text-[color:var(--text-primary)]">{r.symbol}</td>
                        <td className="text-[color:var(--text-primary)]">
                          {r.direction ?? '-'}
                        </td>
                        <td className="numeric-12 text-right text-[color:var(--text-primary)]">
                          {r.price != null ? r.price.toFixed(2) : '-'}
                        </td>
                        <td className="numeric-12 text-right text-[color:var(--text-primary)]">
                          {r.volume != null
                            ? r.volume % 1 === 0
                              ? r.volume.toFixed(0)
                              : r.volume.toFixed(2)
                            : '-'}
                        </td>
                        <td className="text-[color:var(--text-primary)]">
                          {r.status ?? (r.record_type === 'Trade' ? 'Filled' : '-')}
                        </td>
                      </tr>
                    ))
                  )}
                </tbody>
              </table>
            </div>
          </div>
        </div>
      </React.Fragment>
    </PageLayout>
  );
}

