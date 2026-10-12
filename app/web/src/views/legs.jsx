// The legs of a position (Multi-Leg, Risk's portfolio), stored as the JSON the API takes: a summary in
// the sidebar, edited in a window as a table (or as JSON), with a few common strategies to start from.
import { useState } from "preact/hooks";
import * as P from "../lib/payloads.js";
import { params, setParam } from "../lib/store.js";
import { Segmented } from "../ui/controls.jsx";
import { Button, Dialog, Summary } from "../ui/dialog.jsx";

const cell = "h-[26px] w-full min-w-0 rounded-sm border border-line bg-surface px-1.5 text-sm outline-none focus:border-accent";

function read(text) {
  try { return { pos: P.position(text) }; } catch (err) { return { error: err.message }; }
}
let ids = 0;  // a row's key: a replaced row gets new inputs, not the old row's text
const toRow = (l) => ({ id: ++ids, side: Number(l.quantity) < 0 ? "sell" : "buy", option_type: l.option_type === "put" ? "put" : "call",
  quantity: Math.abs(Number(l.quantity ?? 1)), strike: l.strike ?? null, vol: l.vol ?? null, maturity: l.maturity ?? null, forward: l.forward ?? null });
const toLeg = (r) => ({ option_type: r.option_type, strike: r.strike, quantity: (r.side === "sell" ? -1 : 1) * (r.quantity ?? 1),
  ...(r.vol != null ? { vol: r.vol } : {}), ...(r.maturity != null ? { maturity: r.maturity } : {}), ...(r.forward != null ? { forward: r.forward } : {}) });
const toJson = (rows, extra) => {
  const legs = rows.map(toLeg);
  const keep = Object.fromEntries(Object.entries(extra).filter(([, v]) => v !== null && v !== "" && v !== undefined));
  return JSON.stringify(Object.keys(keep).length ? { ...keep, legs } : legs, null, 1);
};

/** A round strike near k (three significant figures, in steps of 5). */
const near = (k) => {
  const step = 5 * 10 ** Math.max(0, Math.floor(Math.log10(Math.abs(k) || 1)) - 2);
  return Math.round(k / step) * step;
};
const leg = (side, option_type, strike) => ({ id: ++ids, side, option_type, quantity: 1, strike: near(strike), vol: null, maturity: null, forward: null });
const PRESETS = [
  ["Straddle", (k) => [leg("buy", "call", k), leg("buy", "put", k)]],
  ["Strangle", (k) => [leg("buy", "call", k * 1.1), leg("buy", "put", k * 0.9)]],
  ["Bull call spread", (k) => [leg("buy", "call", k), leg("sell", "call", k * 1.1)]],
  ["Iron condor", (k) => [leg("buy", "put", k * 0.9), leg("sell", "put", k * 0.95), leg("sell", "call", k * 1.05), leg("buy", "call", k * 1.1)]],
];
export const straddleAt = (k) => PRESETS[0][1](k).map(toLeg);

function NumCell({ value, onChange, placeholder = "–", step = "any" }) {
  const [text, setText] = useState(value ?? "");
  return (
    <input type="number" class={cell} step={step} placeholder={placeholder} value={text}
      onInput={(e) => { const t = e.currentTarget.value; setText(t); onChange(t === "" ? null : Number(t)); }} />
  );
}

function legText(l) {
  const q = Number(l.quantity ?? 1);
  const extra = [l.vol != null && `σ ${(l.vol * 100).toFixed(1)}%`, l.maturity != null && `T ${Number(l.maturity).toFixed(3)}`].filter(Boolean).join(" ");
  return `${q < 0 ? "−" : "+"}${Math.abs(q)} ${l.option_type === "put" ? "P" : "C"} ${Number(l.strike).toLocaleString()}${extra ? `  ${extra}` : ""}`;
}

/** k: the parameter holding the JSON; empty: what an empty box means. */
export function LegsInput({ k, label, empty, title }) {
  const text = params.value[k];
  const { pos, error } = read(text);
  const [open, setOpen] = useState(false);
  const [rows, setRows] = useState([]);
  const [extra, setExtra] = useState({});
  const [view, setView] = useState("table");
  const [json, setJson] = useState("");
  const [problem, setProblem] = useState(null);

  const start = () => {
    setRows((pos?.legs ?? []).map(toRow));
    setExtra({ label: pos?.label ?? null, spot: pos?.spot ?? null, rate: pos?.rate ?? null });
    setView(error ? "json" : "table");
    setJson(text ?? "");
    setProblem(error ?? null);
    setOpen(true);
  };
  const switchView = (v) => {
    if (v === "json") setJson(rows.length ? toJson(rows, extra) : "");
    else {
      const r = read(json);
      if (r.error) return setProblem(r.error);
      setRows((r.pos?.legs ?? []).map(toRow));
      setExtra({ label: r.pos?.label ?? null, spot: r.pos?.spot ?? null, rate: r.pos?.rate ?? null });
    }
    setProblem(null);
    setView(v);
  };
  const apply = () => {
    if (view === "json") {
      const r = read(json);
      if (r.error) return setProblem(r.error);
      setParam(k, json.trim());
    } else {
      if (rows.some((r) => !Number.isFinite(r.strike))) return setProblem("Every leg needs a strike.");
      setParam(k, rows.length ? toJson(rows, extra) : "");
    }
    setOpen(false);
  };
  const update = (i, patch) => setRows(rows.map((r, j) => (j === i ? { ...r, ...patch } : r)));

  return (
    <>
      <Summary label={label} action="Edit" onOpen={start}>
        {error ? <span class="text-xs text-down">Invalid: {error}</span>
          : !pos ? <span class="text-xs text-muted">{empty}</span>
          : (
            <span class="flex flex-col font-mono text-xs leading-5">
              {pos.label && <span class="truncate font-sans text-ink-2">{pos.label}</span>}
              {pos.legs.slice(0, 5).map((l) => <span class="truncate">{legText(l)}</span>)}
              {pos.legs.length > 5 && <span class="text-muted">+{pos.legs.length - 5} more</span>}
            </span>
          )}
      </Summary>
      <Dialog open={open} onClose={() => setOpen(false)} title={title} width="760px"
        footer={<>
          {problem && <span class="mr-auto truncate text-xs text-down">{problem}</span>}
          <Button onClick={() => { setRows([]); setJson(""); setExtra({}); setProblem(null); }} title={empty}>Clear</Button>
          <Button onClick={() => setOpen(false)}>Cancel</Button>
          <Button primary onClick={apply}>Apply</Button>
        </>}>
        <div class="flex flex-col gap-3">
          <div class="flex flex-wrap items-center justify-between gap-2">
            <div class="w-40"><Segmented size="xs" value={view} onChange={switchView} options={[["table", "Table"], ["json", "JSON"]]} /></div>
            {view === "table" && (
              <div class="flex flex-wrap gap-1">
                {PRESETS.map(([name, make]) => (
                  <button type="button" class="h-6 rounded-full border border-line px-2.5 text-xs text-ink-2 hover:border-accent hover:text-accent"
                    onClick={() => { setRows(make(Number(params.value.strike) || 100)); setExtra({}); }} title={`Around the strike ${Number(params.value.strike).toLocaleString()}`}>{name}</button>
                ))}
              </div>
            )}
          </div>
          {view === "json" ? (
            <textarea rows={14} spellcheck={false} class={`${cell} h-auto resize-y py-1.5 font-mono text-xs`} value={json}
              placeholder='[{"option_type":"call","strike":82000,"quantity":1}]' onInput={(e) => setJson(e.currentTarget.value)} />
          ) : (
            <>
              <div class="overflow-x-auto">
                <table class="w-full min-w-[620px] border-separate border-spacing-x-1 border-spacing-y-1 text-sm">
                  <thead>
                    <tr class="text-left text-xs text-muted">
                      <th class="w-28 font-normal">Side</th><th class="w-28 font-normal">Type</th><th class="font-normal">Strike</th><th class="w-16 font-normal">Qty</th>
                      <th class="w-20 font-normal" title="This leg's vol (default: σ)">σ</th><th class="w-20 font-normal" title="This leg's maturity in years (default: T)">T</th>
                      <th class="w-24 font-normal" title="This leg's forward (default: from spot and r)">Forward</th><th class="w-6" />
                    </tr>
                  </thead>
                  <tbody>
                    {rows.map((r, i) => (
                      <tr key={r.id}>
                        <td><Segmented size="xs" value={r.side} onChange={(v) => update(i, { side: v })} options={[["buy", "Buy"], ["sell", "Sell"]]} /></td>
                        <td><Segmented size="xs" value={r.option_type} onChange={(v) => update(i, { option_type: v })} options={[["call", "Call"], ["put", "Put"]]} /></td>
                        <td><NumCell value={r.strike} onChange={(v) => update(i, { strike: v })} /></td>
                        <td><NumCell value={r.quantity} onChange={(v) => update(i, { quantity: v })} /></td>
                        <td><NumCell value={r.vol} onChange={(v) => update(i, { vol: v })} /></td>
                        <td><NumCell value={r.maturity} onChange={(v) => update(i, { maturity: v })} /></td>
                        <td><NumCell value={r.forward} onChange={(v) => update(i, { forward: v })} /></td>
                        <td><button type="button" class="text-muted hover:text-down" title="Remove" onClick={() => setRows(rows.filter((_, j) => j !== i))}>✕</button></td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
              <div class="flex items-center justify-between gap-3">
                <button type="button" class="h-7 rounded-sm border border-dashed border-line px-3 text-sm text-ink-2 hover:border-accent hover:text-accent"
                  onClick={() => setRows([...rows, leg("buy", "call", Number(params.value.strike) || 100)])}>+ Add leg</button>
                <p class="text-xs text-muted">{rows.length ? "Empty σ, T, forward: the global inputs." : empty}</p>
              </div>
              {(extra.label || extra.spot != null || extra.rate != null) && (
                <p class="text-xs text-muted">From the Screener: {extra.label} · spot {extra.spot ?? "–"} · rate {extra.rate ?? "–"} (these replace the global spot and r)</p>
              )}
            </>
          )}
        </div>
      </Dialog>
    </>
  );
}
