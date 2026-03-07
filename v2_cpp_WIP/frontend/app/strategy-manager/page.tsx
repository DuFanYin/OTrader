'use client';

import React, { useCallback, useEffect, useState } from 'react';
import { api } from '@/lib/api';
import PageLayout from '@/app/components/PageLayout';
import TerminalSelect from '@/app/components/TerminalSelect';

interface StrategyRow {
  strategy_name: string;
  class_name: string;
  portfolio: string;
  status: string;
}

type StrategySetting = Record<string, number>;

export default function StrategyManagerPage() {
  const [strategies, setStrategies] = useState<StrategyRow[]>([]);
  const [loadingStrategies, setLoadingStrategies] = useState(false);
  const [selectedStrategy, setSelectedStrategy] = useState<string | null>(null);
  const [strategyClasses, setStrategyClasses] = useState<string[]>([]);
  const [selectedClass, setSelectedClass] = useState<string>('');
  const [portfolios, setPortfolios] = useState<string[]>([]);
  const [selectedPortfolio, setSelectedPortfolio] = useState<string>('');
  const [settingsByClass, setSettingsByClass] = useState<Record<string, StrategySetting>>({});
  const [configOpen, setConfigOpen] = useState(false);
  const [configForClass, setConfigForClass] = useState<string | null>(null);
  const [pendingSetting, setPendingSetting] = useState<{ key: string; value: string }[]>([]);

  const fetchStrategies = useCallback(async () => {
    setLoadingStrategies(true);
    try {
      const data = await api.get<{ strategies?: StrategyRow[] }>('/api/strategies');
      const list: StrategyRow[] = data.strategies || [];
      setStrategies(list);
      setSelectedStrategy((prev) => prev || (list[0]?.strategy_name ?? null));
    } catch (e) {
      console.error('Error loading strategies', e);
    } finally {
      setLoadingStrategies(false);
    }
  }, []);

  const fetchStrategyClasses = useCallback(async () => {
    try {
      const data = await api.get<{ classes?: string[] }>('/api/strategies/meta/strategy-classes');
      const classes: string[] = data.classes || [];
      setStrategyClasses(classes);
      setSelectedClass((prev) => prev || (classes[0] ?? ''));
    } catch (e) {
      console.warn('Failed to load strategy classes', e);
      setStrategyClasses([]);
      setSelectedClass('');
    }
  }, []);

  useEffect(() => {
    void fetchStrategies();
    void fetchStrategyClasses();
    (async () => {
      try {
        const data = await api.get<{ portfolios?: string[] }>('/api/strategies/meta/portfolios');
        const pf: string[] = data.portfolios || [];
        setPortfolios(pf);
        setSelectedPortfolio((prev) => prev || (pf[0] ?? ''));
      } catch {
        // ignore
      }
    })();
  }, [fetchStrategies, fetchStrategyClasses]);

  const withSelected = (fn: (name: string) => Promise<void>) => async () => {
    if (!selectedStrategy) {
      alert('Please select a strategy first');
      return;
    }
    await fn(selectedStrategy);
    await fetchStrategies();
  };

  const handleAddStrategy = async () => {
    const strategyClass = selectedClass;
    if (!strategyClass) {
      alert('Please select a strategy class first');
      return;
    }
    const portfolioName = selectedPortfolio || window.prompt('Portfolio name?') || '';
    if (!portfolioName) return;
    const setting = settingsByClass[strategyClass] ?? {};
    try {
      await api.post('/api/strategies', {
        strategy_class: strategyClass,
        portfolio_name: portfolioName,
        setting,
      });
      await fetchStrategies();
    } catch (e) {
      console.error('Add strategy error', e);
    }
  };

  const callStrategyEndpoint = async (name: string, action: 'init' | 'start' | 'stop' | 'remove' | 'delete') => {
    const path = `/api/strategies/${encodeURIComponent(name)}`;
    if (action === 'init') {
      await api.post(`${path}/init`);
    } else if (action === 'start') {
      await api.post(`${path}/start`);
    } else if (action === 'stop') {
      await api.post(`${path}/stop`);
    } else if (action === 'remove') {
      await api.delete(`${path}/remove`);
    } else {
      await api.delete(`${path}/delete`);
    }
  };

  const handleOpenConfig = async () => {
    if (!selectedClass) {
      alert('Please select a strategy class first');
      return;
    }
    const strategyClass = selectedClass;
    setConfigForClass(strategyClass);
    setConfigOpen(true);
    // Fetch default settings from engine (strategy_config.json); merge with any local edits
    let defaults: StrategySetting = {};
    try {
      const data = await api.get<{ settings?: StrategySetting }>(
        `/api/strategies/meta/settings?class=${encodeURIComponent(strategyClass)}`
      );
      if (data.settings && typeof data.settings === 'object') {
        defaults = data.settings;
      }
    } catch {
      // live engine may be down; continue with local state only
    }
    const existing = settingsByClass[strategyClass] ?? {};
    const merged: StrategySetting = { ...defaults, ...existing };
    const rows =
      Object.keys(merged).length === 0
        ? []
        : Object.entries(merged).map(([key, value]) => ({
            key,
            value: String(value),
          }));
    setPendingSetting(rows);
  };

  const handleConfigChangeValue = (index: number, nextValue: string) => {
    setPendingSetting((rows) => {
      const next = [...rows];
      next[index] = { ...next[index], value: nextValue };
      return next;
    });
  };

  const handleConfigApply = () => {
    if (!configForClass) {
      setConfigOpen(false);
      return;
    }
    const setting: StrategySetting = {};
    for (const { key, value } of pendingSetting) {
      const trimmedKey = key.trim();
      if (!trimmedKey) continue;
      const num = Number(value);
      if (Number.isFinite(num)) {
        setting[trimmedKey] = num;
      }
    }
    setSettingsByClass((prev) => ({
      ...prev,
      [configForClass]: setting,
    }));
    setConfigOpen(false);
  };

  return (
    <PageLayout title="Strategy Manager">
      <div className="h-full flex flex-col min-h-0 panel p-3 space-y-3 flex-1 min-h-0">
          <p className="text-xs uppercase tracking-wide text-[color:var(--text-muted)]">
            System Control
          </p>

          {/* Row 1: Strategy class (left) + system-level controls (right) */}
          <div className="flex flex-wrap items-center justify-between gap-3">
            <div className="flex items-center gap-2">
              <span className="form-label">
                Strategy Class
              </span>
              <TerminalSelect
                value={selectedClass}
                onValueChange={setSelectedClass}
                placeholder="Select..."
                className="min-w-[200px]"
                options={strategyClasses.map((cls) => ({ value: cls, label: cls }))}
              />
              <button
                type="button"
                onClick={handleOpenConfig}
                className={`h-7 px-3 btn disabled:opacity-40 disabled:cursor-not-allowed ${
                  selectedClass ? 'btn-primary' : 'btn-ghost'
                }`}
                disabled={!selectedClass}
              >
                Config
              </button>
            </div>
            <div className="flex flex-wrap items-center gap-2">
              <button
                type="button"
                onClick={handleAddStrategy}
                className="h-7 px-4 btn btn-primary"
              >
                Add Strategy
              </button>
              <button
                type="button"
                disabled
                className="h-7 px-4 border border-[color:var(--border-subtle)] text-xs text-[color:var(--text-muted)] cursor-not-allowed"
                title="Restore not supported in C++ live mode"
              >
                Restore Strategy
              </button>
              <button
                type="button"
                onClick={fetchStrategies}
                className="h-7 px-4 btn btn-primary"
              >
                Refresh Strategies
              </button>
            </div>
          </div>

          {/* Row 2: Portfolio (left) + per-strategy lifecycle controls (right) */}
          <div className="flex flex-wrap items-center justify-between gap-3">
            <div className="flex items-center gap-2">
              <span className="form-label">
                Portfolio
              </span>
              <TerminalSelect
                value={selectedPortfolio}
                onValueChange={setSelectedPortfolio}
                placeholder="Custom..."
                className="min-w-[160px]"
                options={portfolios.map((pf) => ({ value: pf, label: pf }))}
              />
            </div>
            <div className="flex flex-wrap items-center gap-2">
              <button
                type="button"
                onClick={withSelected((name) => callStrategyEndpoint(name, 'init'))}
                className="h-7 px-3 btn btn-primary"
              >
                Initialize
              </button>
              <button
                type="button"
                onClick={withSelected((name) => callStrategyEndpoint(name, 'start'))}
                className="h-7 px-3 btn btn-primary"
              >
                Start
              </button>
              <button
                type="button"
                onClick={withSelected((name) => callStrategyEndpoint(name, 'stop'))}
                className="h-7 px-3 btn btn-primary"
              >
                Stop
              </button>
              <button
                type="button"
                onClick={withSelected((name) => callStrategyEndpoint(name, 'remove'))}
                className="h-7 px-3 btn btn-danger"
              >
                Remove
              </button>
              <button
                type="button"
                onClick={withSelected((name) => callStrategyEndpoint(name, 'delete'))}
                className="h-7 px-3 btn btn-danger"
              >
                Delete
              </button>
            </div>
          </div>
          <div className="panel bg-[color:var(--surface-subtle)] flex-1 min-h-0 p-3 overflow-auto">
            {loadingStrategies ? (
              <p className="text-xs text-[color:var(--text-muted)]">Loading strategies...</p>
            ) : (
              <table className="table-terminal">
                <thead>
                  <tr>
                    <th className="py-1 px-2 text-left">Select</th>
                    <th className="py-1 px-2 text-left">Name</th>
                    <th className="py-1 px-2 text-left">Class</th>
                    <th className="py-1 px-2 text-left">Portfolio</th>
                    <th className="py-1 px-2 text-left">Status</th>
                    <th className="py-1 px-2 text-right">uPnL</th>
                    <th className="py-1 px-2 text-right">Position</th>
                    <th className="py-1 px-2 text-right">Delta</th>
                    <th className="py-1 px-2 text-right">Gamma</th>
                    <th className="py-1 px-2 text-right">Theta</th>
                  </tr>
                </thead>
                <tbody>
                  {strategies.length === 0 ? (
                    <tr>
                      <td
                        colSpan={10}
                        className="py-3 px-2 text-center text-[color:var(--text-muted)]"
                      >
                        No strategies found. Use &quot;Add Strategy&quot; to create one.
                      </td>
                    </tr>
                  ) : (
                    strategies.map((s) => {
                      const isSelected = s.strategy_name === selectedStrategy;
                      return (
                        <tr
                          key={s.strategy_name}
                          className={`cursor-pointer ${
                            isSelected ? 'table-row-selected' : 'table-row-hover'
                          }`}
                          onClick={() => setSelectedStrategy(s.strategy_name)}
                        >
                          <td className="py-1 px-2">
                            <input
                              type="radio"
                              checked={isSelected}
                              onChange={() => setSelectedStrategy(s.strategy_name)}
                            />
                          </td>
                          <td className="py-1 px-2 text-[color:var(--text-primary)]">
                            {s.strategy_name}
                          </td>
                          <td className="py-1 px-2 text-[color:var(--text-soft)]">{s.class_name}</td>
                          <td className="py-1 px-2 text-[color:var(--text-soft)]">{s.portfolio}</td>
                          <td className="py-1 px-2">
                            <span
                              className={`inline-flex items-center rounded px-2 py-0.5 text-[10px] uppercase tracking-wide ${
                                s.status === 'running'
                                  ? 'text-[color:var(--state-success)]'
                                  : s.status === 'error'
                                    ? 'text-[color:var(--state-error)]'
                                    : s.status === 'stopped'
                                      ? 'text-[color:var(--text-primary)]'
                                      : 'text-[color:var(--text-soft)]'
                              }`}
                            >
                              {s.status}
                            </span>
                          </td>
                          {/* The following columns are reserved for future live metrics (uPnL / greeks / position). */}
                          <td className="py-1 px-2 numeric-12 text-right text-[color:var(--text-soft)]">
                            -
                          </td>
                          <td className="py-1 px-2 numeric-12 text-right text-[color:var(--text-soft)]">
                            -
                          </td>
                          <td className="py-1 px-2 numeric-12 text-right text-[color:var(--text-soft)]">
                            -
                          </td>
                          <td className="py-1 px-2 numeric-12 text-right text-[color:var(--text-soft)]">
                            -
                          </td>
                          <td className="py-1 px-2 numeric-12 text-right text-[color:var(--text-soft)]">
                            -
                          </td>
                        </tr>
                      );
                    })
                  )}
                </tbody>
              </table>
            )}
          </div>

        {configOpen && (
          <div className="fixed inset-0 z-40 flex items-center justify-center bg-black/60">
            <div
              className="panel max-w-lg w-full mx-4 p-4"
              style={{ backgroundColor: 'var(--surface-subtle)' }}
            >
              <div className="flex items-center justify-between mb-3">
                <h2 className="text-panel-title text-[color:var(--text-primary)]">
                  Strategy Settings
                </h2>
              </div>
              <p className="text-xs text-[color:var(--text-muted)] mb-3">
                Configure numeric parameters for the selected strategy class (from engine defaults).
              </p>
              <div className="space-y-2 mb-3">
                {pendingSetting.length === 0 && (
                  <p className="text-xs text-[color:var(--text-soft)]">No parameters for this strategy class.</p>
                )}
                {pendingSetting.map((row, index) => {
                  return (
                    <div key={index} className="flex items-center gap-2">
                      <span className="form-label min-w-[120px] shrink-0">{row.key}</span>
                      <input
                        type="number"
                        value={row.value}
                        onChange={(e) => handleConfigChangeValue(index, e.target.value)}
                        placeholder="0"
                        className="w-28 form-input text-xs"
                      />
                    </div>
                  );
                })}
              </div>
              <div className="flex justify-end gap-2">
                <button
                  type="button"
                  className="h-7 px-4 btn btn-ghost"
                  onClick={() => setConfigOpen(false)}
                >
                  Cancel
                </button>
                <button
                  type="button"
                  className="h-7 px-4 btn btn-primary"
                  onClick={handleConfigApply}
                >
                  Apply
                </button>
              </div>
            </div>
          </div>
        )}
      </div>
    </PageLayout>
  );
}

