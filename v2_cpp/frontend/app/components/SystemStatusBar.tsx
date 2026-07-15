'use client';

import { useEffect, useState } from 'react';
import { api } from '@/lib/api';

interface LiveStatus {
  status: string;
  connected: boolean;
  detail?: string;
}

interface SystemStatusResponse {
  backend: { status: string };
  live: LiveStatus;
}

export default function SystemStatusBar() {
  const [status, setStatus] = useState<SystemStatusResponse | null>(null);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);
  const [restarting, setRestarting] = useState(false);

  const fetchStatus = async () => {
    try {
      const data = await api.get<SystemStatusResponse>('/api/system/status');
      setStatus(data);
      setError(null);
    } catch {
      setError('Failed to fetch system status');
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    fetchStatus();
    const id = setInterval(fetchStatus, 3000);
    return () => clearInterval(id);
  }, []);

  const handleRestart = async () => {
    setRestarting(true);
    try {
      await api.post('/api/system/restart_live');
      fetchStatus();
    } catch {
      setError('Failed to restart live engine');
    } finally {
      setRestarting(false);
    }
  };

  const live = status?.live;

  return (
    <div className="flex items-center justify-between border-b border-[color:var(--border-subtle)] bg-[color:var(--surface-subtle)] px-4 py-2 text-xs text-[color:var(--text-soft)]">
      <div className="flex items-center gap-4">
        <div className="flex items-center gap-1.5">
          <span
            className={`inline-block h-2 w-2 rounded-full ${
              status?.backend.status === 'running'
                ? 'bg-[color:var(--state-success)]'
                : 'bg-[color:var(--state-error)]'
            }`}
          />
          <span>Backend</span>
        </div>
        <div className="flex items-center gap-1.5">
          <span
            className={`inline-block h-2 w-2 rounded-full ${
              live?.status === 'running' && live?.connected
                ? 'bg-[color:var(--state-success)]'
                : live?.status === 'error'
                  ? 'bg-[color:var(--state-error)]'
                  : 'bg-[color:var(--state-warning)]'
            }`}
          />
          <span>Live</span>
          {loading ? (
            <span className="text-[color:var(--text-muted)]">loading...</span>
          ) : live?.detail ? (
            <span className="max-w-xs truncate text-[10px] text-[color:var(--text-muted)]">
              {live.detail}
            </span>
          ) : null}
        </div>
        {error && (
          <span className="text-[10px] text-[color:var(--state-error)]">{error}</span>
        )}
      </div>
      <div className="flex items-center gap-2">
        <button
          type="button"
          onClick={handleRestart}
          disabled={restarting}
          className="btn btn-primary px-2 py-1 font-medium disabled:cursor-not-allowed disabled:opacity-60"
        >
          {restarting ? 'Restarting…' : 'Restart live'}
        </button>
      </div>
    </div>
  );
}

