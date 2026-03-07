'use client';

import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useState,
  type ReactNode,
} from 'react';

import { api, getWsLogsUrl } from '@/lib/api';

const MAX_LOGS = 500;

type LogContextValue = {
  logs: string[];
  appendLog: (line: string) => void;
  clearLogs: () => Promise<void>;
};

const LogContext = createContext<LogContextValue | null>(null);

function parseLogLine(raw: string): string {
  try {
    const obj = JSON.parse(raw) as {
      src?: string;
      time?: string;
      level_str?: string;
      gateway?: string;
      msg?: string;
    };
    if (obj && obj.msg) {
      const ts = obj.time ?? new Date().toISOString().slice(11, 19);
      const lvl = (obj.level_str ?? 'INFO').padEnd(7);
      const gw = (obj.gateway ?? (obj.src === 'live' ? 'Live' : 'Backend')).padEnd(10);
      return `${ts} | ${lvl} | ${gw} | ${obj.msg}`;
    }
  } catch {
    // not JSON
  }
  return raw;
}

export function LogProvider({ children }: { children: ReactNode }) {
  const [logs, setLogs] = useState<string[]>([]);

  const appendLog = useCallback((line: string) => {
    setLogs((prev) => [...prev.slice(-(MAX_LOGS - 1)), line]);
  }, []);

  const clearLogs = useCallback(async () => {
    try {
      await api.post('/api/logs/clear');
    } catch {
      // still clear local so UI updates
    }
    setLogs([]);
  }, []);

  useEffect(() => {
    let ws: WebSocket | null = null;
    try {
      ws = new WebSocket(getWsLogsUrl());
      ws.onmessage = (event) => {
        const raw = String(event.data ?? '');
        appendLog(parseLogLine(raw));
      };
    } catch {
      // ignore
    }
    return () => {
      if (ws && ws.readyState === WebSocket.OPEN) ws.close();
    };
  }, [appendLog]);

  const value = useMemo<LogContextValue>(
    () => ({ logs, appendLog, clearLogs }),
    [logs, appendLog, clearLogs]
  );

  return <LogContext.Provider value={value}>{children}</LogContext.Provider>;
}

export function useLogs(): LogContextValue {
  const ctx = useContext(LogContext);
  if (!ctx) throw new Error('useLogs must be used within LogProvider');
  return ctx;
}
