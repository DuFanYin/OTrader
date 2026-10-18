// Building blocks of the results: cards, key-value lists, tables, buttons, run states, notes, warnings.
import { toast } from "./toast.jsx";
import { ENGINE_DOWN } from "../lib/api.js";
import { fmt, ms } from "../lib/format.js";
import { results } from "../lib/store.js";

/** A result card. `wide` spans the whole results grid; `meta` and `actions` sit at the right of the title. */
export function Card({ title, meta, wide, children, actions }) {
  return (
    <section class={`flex min-w-0 flex-col rounded-md border border-line bg-surface shadow-[var(--shadow)] ${wide ? "col-span-full" : ""}`}>
      <header class="flex min-h-9 flex-wrap items-center justify-between gap-x-3 gap-y-1.5 px-3 pt-2">
        <h2 class="min-w-0 truncate text-sm font-semibold">{title}</h2>
        <div class="flex flex-wrap items-center justify-end gap-2 text-xs text-muted">{meta}{actions}</div>
      </header>
      <div class="flex min-w-0 flex-col gap-3 px-3 pt-1 pb-3">{children}</div>
    </section>
  );
}

/** A small heading inside a card. */
export const Sub = ({ children }) => <h3 class="text-2xs font-semibold tracking-[0.08em] text-muted uppercase">{children}</h3>;

/** Value tones: good / bad / warn colouring for a number or text. */
export const Good = ({ children }) => <span class="text-up">{children}</span>;
export const Bad = ({ children }) => <span class="text-down">{children}</span>;
export const Warn = ({ children }) => <span class="text-warn">{children}</span>;
export function tone(value, ok, text = fmt(value)) {
  if (value == null) return "–";
  return ok ? <Good>{text}</Good> : <Warn>{text}</Warn>;
}

/** A gain or a loss, coloured by its sign (zero is neither). */
export function Signed({ value, format = fmt }) {
  const v = Number(value);
  if (value == null || !Number.isFinite(v)) return "–";
  return <span class={v > 0 ? "text-up" : v < 0 ? "text-down" : ""}>{format(value)}</span>;
}

export function Badge({ kind = "muted", children }) {
  const cls = { good: "bg-up-soft text-up", bad: "bg-down-soft text-down", warn: "bg-warn-soft text-warn", muted: "bg-surface-2 text-ink-2",
    accent: "bg-accent-soft text-accent" }[kind];
  return <span class={`inline-flex h-5 items-center rounded-full px-2 text-xs font-medium ${cls}`}>{children}</span>;
}

/** Label / value pairs in two columns (one on a phone; four on a wide card on a wide screen). Values that are
 * not JSX go through fmt(). */
export function Kv({ rows, cols = 2 }) {
  const items = rows.filter(Boolean);
  if (!items.length) return <Empty />;
  return (
    <dl class={`grid gap-x-6 ${{ 1: "", 2: "sm:grid-cols-2", 3: "sm:grid-cols-3", 4: "sm:grid-cols-2 xl:grid-cols-4" }[cols]}`}>
      {items.map(([k, v]) => (
        <div class="flex min-w-0 items-baseline justify-between gap-3 border-b border-line/70 py-[3px] text-sm">
          <dt class="max-w-[60%] shrink-0 truncate text-ink-2">{k}</dt>
          <dd class="m-0 min-w-0 text-right font-medium tabular-nums [overflow-wrap:anywhere]">{isNode(v) ? v : fmt(v)}</dd>
        </div>
      ))}
    </dl>
  );
}

const isNode = (v) => v !== null && typeof v === "object" && ("type" in v || Array.isArray(v));

/** A table, its rows ruled and no box of its own (the card is the box). cols: [{ key, label, fmt?, align?, cls? }]; rows: objects. */
export function Table({ cols, rows, onRow, selected, compact, maxHeight, empty }) {
  if (!rows?.length) return <Empty>{empty}</Empty>;
  return (
    <div class="min-w-0 overflow-auto" style={maxHeight ? { maxHeight } : undefined}>
      <table class="w-full border-collapse text-sm">
        <thead class="sticky top-0 z-[1] border-b border-line-strong bg-surface">
          <tr>{cols.map((c) => <th class={`whitespace-nowrap px-2 py-1 text-xs font-medium text-muted first:pl-0 last:pr-0 ${c.align === "left" ? "text-left" : "text-right"}`}>{c.label ?? c.key}</th>)}</tr>
        </thead>
        <tbody>
          {rows.map((r, i) => (
            <tr onClick={onRow ? () => onRow(r, i) : undefined}
              class={`border-t border-line ${onRow ? "cursor-pointer hover:bg-surface-2" : ""} ${selected === i ? "bg-accent-soft" : ""}`}>
              {cols.map((c) => {
                const v = c.fmt ? c.fmt(r[c.key], r) : r[c.key];
                const wrap = c.cls?.includes("whitespace-") ? "" : "whitespace-nowrap";  // a column may wrap
                return <td class={`${wrap} px-2 first:pl-0 last:pr-0 ${compact ? "py-0.5" : "py-1"} ${c.align === "left" ? "text-left" : "text-right"} ${c.cls ?? ""}`}>{isNode(v) ? v : fmt(v)}</td>;
              })}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

export const Empty = ({ children = "No data" }) => <p class="text-sm text-muted">{children}</p>;
export const Note = ({ children }) => <p class="text-xs leading-relaxed text-muted">{children}</p>;

/** A plain button for a panel's header. */
export function Button({ children, onClick, primary, title, disabled, type = "button" }) {
  return (
    <button type={type} title={title} onClick={onClick} disabled={disabled}
      class={`h-8 rounded-sm px-3 text-sm transition-colors disabled:cursor-not-allowed disabled:opacity-40 ${primary
        ? "bg-accent font-semibold text-accent-ink hover:opacity-90" : "border border-line text-ink-2 hover:border-line-strong hover:text-ink"}`}>
      {children}
    </button>
  );
}

/** A small button in a card's header. */
export function Action({ children, onClick, title, disabled, danger }) {
  return (
    <button type="button" title={title} disabled={disabled} onClick={onClick}
      class={`h-6 rounded-sm border px-2 text-xs transition-colors disabled:cursor-not-allowed disabled:opacity-40 ${danger
        ? "border-line text-ink-2 hover:border-down hover:text-down" : "border-line text-ink-2 hover:border-line-strong hover:text-ink"}`}>
      {children}
    </button>
  );
}

/** Warnings under a result: a tinted list. */
export function Warnings({ items }) {
  if (!items?.length) return null;
  return (
    <ul class="m-0 flex list-none flex-col gap-1 rounded-sm bg-warn-soft px-2.5 py-2 text-xs text-warn">
      {items.map((w) => <li>⚠ {w}</li>)}
    </ul>
  );
}

/** The warnings of a result (an envelope, a list of them, or an object holding them), once each. */
function warningsOf(data) {
  if (!data || typeof data !== "object") return [];
  if (Array.isArray(data)) return [...new Set(data.flatMap(warningsOf))];
  if (Array.isArray(data.warnings)) return data.warnings;
  return [...new Set(Object.values(data).flatMap((v) => (v && typeof v === "object" && Array.isArray(v.warnings) ? v.warnings : [])))];
}

/** A result card bound to one slot of a mode's results: running, error, or `children(data)` and its warnings. */
export function Slot({ mode, slot, title, wide, meta, children }) {
  const s = results.value[mode]?.[slot];
  if (!s) return null;
  const engine = s.data?.diagnostics?.compute_ms ?? s.data?.diagnostics?.runtime_ms;
  const warnings = s.state === "ok" ? warningsOf(s.data) : [];
  const id = s.state === "ok" ? s.data?.run_id : null;
  const copy = () => navigator.clipboard?.writeText(id).then(() => toast(id, "Run id copied"), () => toast(id, "Run id"));
  return (
    <Card title={title} wide={wide} meta={meta ?? (s.state === "ok" && (
      <>
        {id && <button type="button" class="font-mono text-2xs hover:text-ink" title={`Run ${id}: click to copy (the API's get_run finds it)`} onClick={copy}>#{id.slice(0, 8)}</button>}
        {engine != null && <span title="compute time">{ms(engine)}</span>}
      </>
    ))}>
      {s.state === "running" ? <Running /> : s.state === "error" ? <ErrorText>{s.error}</ErrorText> : children(s.data)}
      <Warnings items={warnings} />
    </Card>
  );
}

export const Running = () => (
  <div class="flex flex-col gap-2 py-1" aria-busy="true">
    {[72, 56, 64].map((w) => <div class="h-3 animate-pulse rounded-sm bg-surface-2" style={{ width: `${w}%` }} />)}
  </div>
);

/** A failure, in red; the engine not answering is a state, shown as one. */
export const ErrorText = ({ children }) => (children === ENGINE_DOWN ? (
  <p class="flex items-center gap-2 py-1 text-sm text-muted">
    <span class="h-1.5 w-1.5 shrink-0 animate-pulse rounded-full bg-warn" />{children}
  </p>
) : (
  <p class="rounded-sm bg-down-soft px-2 py-1.5 font-mono text-xs leading-relaxed break-words text-down">{children}</p>
));
