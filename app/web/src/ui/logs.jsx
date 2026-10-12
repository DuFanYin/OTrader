// The log drawer: a tab at the bottom right that opens the engine's and the backend's log lines, newest at the
// bottom. The tab turns red when an error arrives while it is closed.
import { signal } from "@preact/signals";
import { useEffect, useRef } from "preact/hooks";
import { clearLogs, connected, lines } from "../lib/logs.js";
import { Button } from "./blocks.jsx";
import { toast } from "./toast.jsx";

const open = signal(false);
const seen = signal(0);  // lines read: the ones after it are new

const LEVEL = { ERROR: "text-down", CRITICAL: "text-down", WARNING: "text-warn", WARN: "text-warn", DEBUG: "text-muted" };

export function LogDrawer() {
  const list = lines.value;
  const fresh = list.slice(Math.min(seen.value, list.length));
  const alarm = !open.value && fresh.some((l) => LEVEL[l.level] === "text-down");
  const end = useRef(null);
  useEffect(() => {
    if (open.value) {
      seen.value = list.length;
      end.current?.scrollIntoView({ block: "end" });
    }
  }, [open.value, list.length]);
  return (
    <>
      <button type="button" onClick={() => (open.value = !open.value)} aria-expanded={open.value}
        class={`flex h-9 items-center gap-2 rounded-full border bg-surface px-3.5 text-sm shadow-lg hover:text-ink ${alarm
          ? "border-down text-down" : "border-line text-ink-2 hover:border-line-strong"}`}>
        <span class={`h-1.5 w-1.5 rounded-full ${connected.value ? "bg-up" : "bg-line-strong"}`} title={connected.value ? "Streaming" : "Not connected"} />
        Log <span class="rounded-full bg-surface-2 px-1.5 text-xs text-muted">{fresh.length || list.length}</span>
      </button>
      {open.value && (
        <section class="fixed right-4 bottom-16 z-30 flex h-[min(60vh,560px)] w-[min(960px,calc(100vw-32px))] flex-col rounded-md border border-line bg-surface shadow-2xl">
          <header class="flex h-11 shrink-0 items-center justify-between gap-3 border-b border-line px-3">
            <h2 class="text-sm font-semibold">Log <span class="font-normal text-muted">· the last {list.length} lines</span></h2>
            <div class="flex items-center gap-2">
              {list.length > 0 && <Button onClick={() => clearLogs().catch((e) => toast(e.message, "Clear the log"))}>Clear</Button>}
              <button type="button" class="px-1 text-muted hover:text-ink" onClick={() => (open.value = false)} aria-label="Close">✕</button>
            </div>
          </header>
          <div class="min-h-0 flex-1 overflow-auto px-3 py-2 font-mono text-xs leading-relaxed">
            {list.length === 0 ? <p class="font-sans text-sm text-muted">The engine's and the backend's log lines appear here as they come.</p>
              : list.map((l) => (
                <div class={`flex gap-3 whitespace-pre-wrap break-words ${LEVEL[l.level] ?? "text-ink-2"}`}>
                  <span class="shrink-0 text-muted">{l.time}</span>
                  <span class="w-16 shrink-0">{l.level}</span>
                  <span class="w-20 shrink-0 truncate text-muted">{l.source}</span>
                  <span class="min-w-0">{l.msg}</span>
                </div>
              ))}
            <div ref={end} />
          </div>
        </section>
      )}
    </>
  );
}
