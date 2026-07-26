'use client';

import { useLogs } from '../context/LogContext';

// 固定高度版本：具体 35% 高度由外层 layout 控制，这里只负责在自身区域内滚动日志。
export default function LogDock() {
  const { logs, clearLogs } = useLogs();

  return (
    <section
      className="h-full border-t border-[color:var(--border-subtle)] bg-[color:var(--surface-subtle)] text-[color:var(--text-primary)] flex flex-col"
      aria-label="System logs"
    >
      <div className="h-1 w-full bg-[color:var(--border-subtle)]" />
      <div className="px-3 py-1 flex items-center justify-between text-[11px] uppercase tracking-wide border-b border-[color:var(--border-subtle)]">
        <div className="flex items-center gap-2">
          <span className="text-[color:var(--text-muted)]">System Logs</span>
          <span className="text-[color:var(--text-muted)]">
            {logs.length > 0 ? `${logs.length} lines` : 'idle'}
          </span>
        </div>
        <div className="flex items-center gap-2">
          <button
            type="button"
            onClick={() => void clearLogs()}
            className="h-6 px-2 border border-[color:var(--border-subtle)] text-[11px] text-[color:var(--text-soft)] hover:border-[color:var(--border-strong)] hover:bg-[color:var(--surface-raised)]"
          >
            Clear
          </button>
        </div>
      </div>
      <div className="flex-1 px-3 py-2 overflow-auto log-terminal text-[color:var(--text-soft)]">
        {logs.length === 0 ? (
          <span className="text-[color:var(--text-muted)]">Logs will appear here.</span>
        ) : (
          logs.map((line, i) => {
            const upper = line.toUpperCase();
            const levelClass =
              upper.includes(' ERROR ') || upper.startsWith('ERROR ') || upper.includes('| ERROR')
                ? 'text-[color:var(--state-error)]'
                : upper.includes(' WARN ') || upper.includes('| WARN') || upper.includes(' WARNING')
                  ? 'text-[color:var(--state-warning)]'
                  : 'text-[color:var(--text-soft)]';
            return (
              <div key={i} className={levelClass}>
                {line}
              </div>
            );
          })
        )}
      </div>
    </section>
  );
}

