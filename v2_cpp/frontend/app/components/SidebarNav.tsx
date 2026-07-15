'use client';

import Link from 'next/link';
import { usePathname } from 'next/navigation';
import { useEffect, useState } from 'react';
import { api, getApiBase } from '@/lib/api';
import { NAV_ITEMS } from '@/lib/nav';
import { useLogs } from '@/app/context/LogContext';

interface LiveStatus {
  status: string;
  connected: boolean;
  detail?: string;
}

interface SystemStatusResponse {
  backend: { status: string };
  live: LiveStatus;
}

interface GatewayStatus {
  status: string;
  connected: boolean;
  detail?: string;
}

interface MarketStatus {
  status: string;
  connected: boolean;
  detail?: string;
}

export default function SidebarNav() {
  const pathname = usePathname();
  const { appendLog } = useLogs();
  const [liveStatus, setLiveStatus] = useState<LiveStatus | null>(null);
  const [gatewayStatus, setGatewayStatus] = useState<GatewayStatus | null>(null);
  const [marketStatus, setMarketStatus] = useState<MarketStatus | null>(null);
  const [gwConnecting, setGwConnecting] = useState(false);
  const [mdToggling, setMdToggling] = useState(false);

  useEffect(() => {
    const fetchStatus = async () => {
      try {
        const res = await fetch('http://localhost:8080/api/system/status');
        if (res.ok) {
          const data = (await res.json()) as SystemStatusResponse;
          setLiveStatus(data.live);
        }
      } catch {
        setLiveStatus({ status: 'error', connected: false, detail: 'backend unreachable' });
      }

      try {
        const gwData = await api.get<GatewayStatus>('/api/gateway/status');
        setGatewayStatus(gwData);
      } catch {
        setGatewayStatus({ status: 'error', connected: false, detail: 'backend unreachable' });
      }

      try {
        const mdRes = await fetch('http://localhost:8080/api/market/status');
        if (mdRes.ok) {
          const mdData = (await mdRes.json()) as MarketStatus;
          setMarketStatus(mdData);
        }
      } catch {
        setMarketStatus({ status: 'error', connected: false, detail: 'backend unreachable' });
      }
    };

    fetchStatus();
    const id = setInterval(fetchStatus, 5000);
    return () => clearInterval(id);
  }, []);

  // Live Engine: 只看进程/gRPC 是否 OK（status），不依赖 IBKR 是否已连接
  const liveDotClass =
    liveStatus?.status === 'running'
      ? 'bg-[color:var(--state-success)]'
      : liveStatus?.status === 'error'
        ? 'bg-[color:var(--state-error)]'
        : 'bg-[color:var(--state-warning)]';

  const gwDotClass =
    gatewayStatus?.connected && gatewayStatus?.status === 'running'
      ? 'bg-[color:var(--state-success)]'
      : gatewayStatus?.status === 'error'
        ? 'bg-[color:var(--state-error)]'
        : 'bg-[color:var(--state-warning)]';

  const mdDotClass =
    marketStatus?.connected && marketStatus?.status === 'running'
      ? 'bg-[color:var(--state-success)]'
      : marketStatus?.status === 'error'
        ? 'bg-[color:var(--state-error)]'
        : 'bg-[color:var(--state-warning)]';

  const handleGatewayToggle = async () => {
    if (gwConnecting) return;
    const isOn = gatewayStatus?.connected && gatewayStatus?.status === 'running';
    setGwConnecting(true);
    try {
      const res = await fetch(
        `${getApiBase()}/api/gateway/${isOn ? 'disconnect' : 'connect'}`,
        { method: 'POST' },
      );
      const body = (await res.json().catch(() => ({}))) as Record<string, unknown>;
      const status = (body && body.status) || (res.ok ? 'ok' : 'error');
      const message =
        (body && (body.message || body.detail || body.error)) ||
        (status === 'ok'
          ? isOn
            ? 'Disconnected successfully.'
            : 'Connected successfully.'
          : res.statusText || 'Unknown error');

      appendLog(
        `${status === 'ok' ? 'INFO' : 'ERROR'} | IBKR | ${isOn ? 'disconnect' : 'connect'} | ${status} | ${String(message)}`,
      );
    } catch (e) {
      console.error('Gateway toggle error', e);
      appendLog('ERROR | IBKR | toggle failed');
    } finally {
      setGwConnecting(false);
    }
  };

  const handleMarketToggle = async () => {
    if (mdToggling) return;
    const isOn = marketStatus?.connected && marketStatus?.status === 'running';
    setMdToggling(true);
    try {
      const res = await fetch(
        `${getApiBase()}/api/market/${isOn ? 'stop' : 'start'}`,
        { method: 'POST' },
      );
      const body = (await res.json().catch(() => ({}))) as Record<string, unknown>;
      const status = (body && body.status) || (res.ok ? 'ok' : 'error');
      const message =
        (body && (body.message || body.detail || body.error)) ||
        (status === 'ok'
          ? isOn
            ? 'Market data stopped.'
            : 'Market data started.'
          : res.statusText || 'Unknown error');

      appendLog(
        `${status === 'ok' ? 'INFO' : 'ERROR'} | MarketData | ${isOn ? 'stop' : 'start'} | ${status} | ${String(message)}`,
      );
    } catch (e) {
      console.error('Market data toggle error', e);
      appendLog('ERROR | MarketData | toggle failed');
    } finally {
      setMdToggling(false);
    }
  };

  return (
    <aside className="fixed left-0 top-0 hidden h-screen md:w-[12%] border-r border-[color:var(--border-subtle)] bg-[color:var(--surface-subtle)] px-3 py-4 md:flex md:flex-col">
      <div className="mb-4 px-2 text-center">
        <div className="text-base font-semibold uppercase tracking-wide text-[color:var(--text-primary)]">
          FACTT
        </div>
        <div className="text-sm text-[color:var(--text-muted)]">Trading Control Panel</div>
      </div>
      <div className="mb-4 space-y-2 border-y border-[color:var(--border-subtle)] py-3 text-sm text-[color:var(--text-soft)]">
        <div className="flex items-center gap-3">
          <span className={`inline-block h-2.5 w-2.5 rounded-full ${liveDotClass}`} />
          <span className="font-semibold tracking-wide text-[color:var(--text-primary)]">
            Live Engine
          </span>
        </div>
        <div className="flex items-center justify-between">
          <div className="flex items-center gap-3">
            <span className={`inline-block h-2.5 w-2.5 rounded-full ${gwDotClass}`} />
            <span className="font-semibold tracking-wide text-[color:var(--text-primary)]">
              IBKR
            </span>
          </div>
          <button
            type="button"
            onClick={handleGatewayToggle}
            disabled={gwConnecting}
            title={
              gatewayStatus?.connected && gatewayStatus?.status === 'running'
                ? 'Disconnect IBKR Gateway'
                : 'Connect IBKR Gateway'
            }
            className="inline-flex h-5 w-5 items-center justify-center border border-[color:var(--border-subtle)] text-[11px] text-[color:var(--text-primary)] hover:border-[color:var(--border-strong)] hover:bg-[color:var(--surface-raised)] disabled:opacity-40 disabled:cursor-not-allowed"
          >
            {gatewayStatus?.connected && gatewayStatus?.status === 'running' ? '⏻' : '+'}
          </button>
        </div>
        <div className="flex items-center justify-between">
          <div className="flex items-center gap-3">
            <span className={`inline-block h-2.5 w-2.5 rounded-full ${mdDotClass}`} />
            <span className="font-semibold tracking-wide text-[color:var(--text-primary)]">
              Market Data
            </span>
          </div>
          <button
            type="button"
            onClick={handleMarketToggle}
            disabled={mdToggling}
            title={
              marketStatus?.connected && marketStatus?.status === 'running'
                ? 'Stop Market Data'
                : 'Start Market Data'
            }
            className="inline-flex h-5 w-5 items-center justify-center border border-[color:var(--border-subtle)] text-[11px] text-[color:var(--text-primary)] hover:border-[color:var(--border-strong)] hover:bg-[color:var(--surface-raised)] disabled:opacity-40 disabled:cursor-not-allowed"
          >
            {marketStatus?.connected && marketStatus?.status === 'running' ? '⏻' : '+'}
          </button>
        </div>
      </div>
      <nav className="flex-1 mt-1">
        <ul className="space-y-0.5">
          {NAV_ITEMS.map((item) => {
            const isActive =
              item.href === pathname || pathname.startsWith(item.href);
            return (
              <li key={item.href}>
                <Link
                  href={item.href}
                  className={`flex items-center justify-center px-3 py-2 text-sm font-medium text-center transition-colors border-l-2 ${
                    isActive
                      ? 'border-l-[color:var(--border-strong)] text-[color:var(--text-primary)] bg-transparent'
                      : 'border-l-transparent text-[color:var(--text-soft)] hover:bg-[color:var(--surface-subtle)] hover:text-[color:var(--text-primary)]'
                  }`}
                >
                  {item.label}
                </Link>
              </li>
            );
          })}
        </ul>
      </nav>
    </aside>
  );
}


