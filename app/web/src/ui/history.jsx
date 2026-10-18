// The history drawer: a tab at the bottom right that opens the runs made in this browser. A row loads its results
// back; two ticked rows can be compared.
import { signal } from "@preact/signals";
import { useState } from "preact/hooks";
import { fmt, pct } from "../lib/format.js";
import { clearHistory, compare, forget, history, load } from "../lib/history.js";
import { Dialog, Button } from "./dialog.jsx";
import { Table } from "./blocks.jsx";
import { toast } from "./toast.jsx";

const open = signal(false);
const picked = signal([]);
const comparison = signal(null);  // { a, b, rows } while the comparison is open

const when = (t) => {
  const d = new Date(t), today = new Date().toDateString() === d.toDateString();
  return d.toLocaleString([], today ? { hour: "2-digit", minute: "2-digit" } : { month: "short", day: "numeric", hour: "2-digit", minute: "2-digit" });
};
const value = (v) => (typeof v === "number" ? fmt(v) : String(v));

function pick(id) {
  const p = picked.value;
  picked.value = p.includes(id) ? p.filter((x) => x !== id) : [...p.slice(-1), id];
}

async function startCompare() {
  const chosen = picked.value.map((id) => history.value.find((e) => e.id === id)).filter(Boolean);
  if (chosen.length < 2) return toast("Tick two runs first.", "Compare");
  const [a, b] = chosen.sort((x, y) => x.at - y.at);
  try {
    comparison.value = { a, b, rows: await compare(a, b) };
  } catch (err) {
    toast(err.message, "Compare");
  }
}

export function HistoryDrawer({ views, select }) {
  const [loading, setLoading] = useState(null);
  const list = history.value;
  const label = (id) => views[id]?.label ?? id;
  async function show(entry) {
    setLoading(entry.id);
    select(entry.mode);
    await load(entry);
    setLoading(null);
    open.value = false;
  }
  return (
    <>
      <button type="button" onClick={() => (open.value = !open.value)} aria-expanded={open.value}
        class="flex h-9 items-center gap-2 rounded-full border border-line bg-surface px-3.5 text-sm text-ink-2 shadow-lg hover:border-line-strong hover:text-ink">
        History <span class="rounded-full bg-surface-2 px-1.5 text-xs text-muted">{list.length}</span>
      </button>
      {open.value && (
        <section class="fixed right-4 bottom-16 z-30 flex max-h-[60vh] w-[min(760px,calc(100vw-32px))] flex-col rounded-md border border-line bg-surface shadow-2xl">
          <header class="flex h-11 shrink-0 items-center justify-between gap-3 border-b border-line px-3">
            <h2 class="text-sm font-semibold">Runs in this browser</h2>
            <div class="flex items-center gap-2">
              <Button onClick={startCompare} title="Tick two runs to compare them">
                Compare{picked.value.length ? ` (${picked.value.length}/2)` : ""}
              </Button>
              {list.length > 0 && <Button onClick={() => { if (confirm("Forget every run in this browser?")) { clearHistory(); picked.value = []; } }}>Clear</Button>}
              <button type="button" class="px-1 text-muted hover:text-ink" onClick={() => (open.value = false)} aria-label="Close">✕</button>
            </div>
          </header>
          <div class="min-h-0 overflow-auto">
            {list.length === 0 ? (
              <p class="p-4 text-sm text-muted">Every run you make is listed here; click one to see its results again.</p>
            ) : (
              <table class="w-full border-collapse text-sm">
                <tbody>
                  {list.map((e) => (
                    <tr class="cursor-pointer border-b border-line/70 hover:bg-surface-2" onClick={() => show(e)}>
                      <td class="w-8 py-1.5 pl-3" onClick={(ev) => ev.stopPropagation()}>
                        <input type="checkbox" checked={picked.value.includes(e.id)} onChange={() => pick(e.id)} aria-label="Pick to compare" />
                      </td>
                      <td class="py-1.5 pr-3 whitespace-nowrap text-muted tabular-nums">{when(e.at)}</td>
                      <td class="py-1.5 pr-3 font-medium whitespace-nowrap">{label(e.mode)}</td>
                      <td class="py-1.5 pr-3 text-xs text-ink-2">{(e.inputs || []).map(([k, v]) => `${k} ${value(v)}`).join(" · ")}</td>
                      <td class="py-1.5 pr-3 text-right text-xs whitespace-nowrap">{e.headline ? <><span class="text-muted">{e.headline[0]}</span> {value(e.headline[1])}</> : ""}</td>
                      <td class="py-1.5 pr-2 text-right whitespace-nowrap" onClick={(ev) => ev.stopPropagation()}>
                        {loading === e.id ? <span class="text-xs text-muted">loading…</span>
                          : <button type="button" class="px-1 text-muted hover:text-down" title="Forget this run" onClick={() => forget(e.id)}>✕</button>}
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            )}
          </div>
        </section>
      )}
      <Comparison label={label} />
    </>
  );
}

function Comparison({ label }) {
  const c = comparison.value;
  if (!c) return null;
  const close = () => { comparison.value = null; };
  return (
    <Dialog open onClose={close} title={`${label(c.a.mode)} ${when(c.a.at)} → ${label(c.b.mode)} ${when(c.b.at)}`} width="860px"
      footer={<Button onClick={close} primary>Done</Button>}>
      {c.rows.length === 0 ? <p class="text-sm text-muted">The two runs have the same inputs and results.</p> : (
        <Table compact rows={c.rows} cols={[
          { key: "slot", label: "Card", align: "left" }, { key: "part", label: "", align: "left", cls: "text-muted" },
          { key: "key", label: "Field", align: "left" }, { key: "a", label: "Earlier", fmt: value }, { key: "b", label: "Later", fmt: value },
          { key: "change", label: "Change", fmt: (v) => (v == null ? "" : fmt(v)) },
          { key: "rel", label: "%", fmt: (v) => (v == null ? "" : pct(v, 2)) },
        ]} />
      )}
    </Dialog>
  );
}
