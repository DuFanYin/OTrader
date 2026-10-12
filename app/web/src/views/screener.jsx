// Screener: strategies built from the live Deribit chain (or a cached snapshot), or from a day the trading
// engine recorded or backtests (SPXW, SPY: a day and a time), filtered and ranked; or the chain itself.
// A strategy can be sent to Multi-Leg or Risk; one built on an engine's day, backtested as a rule over its days.
import { signal } from "@preact/signals";
import { useState } from "preact/hooks";
import { get, post } from "../lib/api.js";
import { bound, fixed, fmt, pct, usd } from "../lib/format.js";
import * as P from "../lib/payloads.js";
import { begin, fill } from "../lib/runner.js";
import { params, results, setParam } from "../lib/store.js";
import { Badge, Bad, Good, Kv, Note, Slot, Sub, Table } from "../ui/blocks.jsx";
import { Chips, Group, Num, Seg, Select, SmallButton, Text, Toggle } from "../ui/controls.jsx";
import { Button, Dialog, DialogGroup, Summary } from "../ui/dialog.jsx";
import { LineChart } from "../charts/LineChart.jsx";
import { ComboBacktest, canBacktest } from "./combo_backtest.jsx";

const ID = "screener";
const snapshots = signal([]);
const underlyings = signal(["BTC", "ETH"]);
const DERIBIT = ["BTC", "ETH"];
const isDeribit = (u) => DERIBIT.includes(u);

export async function loadSnapshots() {
  try {
    const d = await get(`api/lab/screener/snapshots?currency=${params.value.scrCurrency}`);
    if (d.result_summary?.underlyings) underlyings.value = d.result_summary.underlyings;
    snapshots.value = (d.result_summary?.snapshots || []).map((s) => {
      if (s.day) return [s.snapshot_id, s.day];
      const m = /_(\d{4})(\d{2})(\d{2})_(\d{2})(\d{2})(\d{2})$/.exec(s.snapshot_id);
      return [s.snapshot_id, m ? `${m[1]}-${m[2]}-${m[3]} ${m[4]}:${m[5]} UTC` : s.snapshot_id];
    });
  } catch { /* the list is optional */ }
}

function pickUnderlying(v) {
  setParam("scrCurrency", v);
  setParam("scrSnapshot", "");
  // no open interest or DVOL off Deribit
  if (!isDeribit(v)) {
    setParam("scrMinOi", null);
    if (params.value.scrModelVol === "dvol") setParam("scrModelVol", "surface");
  }
  setTimeout(loadSnapshots);
}

const Range = ({ k, label, hint }) => (
  <>
    <Num k={`${k}Lo`} label={`${label} ≥`} hint={hint} />
    <Num k={`${k}Hi`} label={`${label} ≤`} hint={hint} />
  </>
);

const MODEL_VOLS = { mark: "mark", dvol: "DVOL", surface: "surface", heston: "Heston", none: "none" };
const RANK_KEYS = { rr: "RR", edge: "edge", cost: "cost", credit: "credit", gain: "gain", loss: "loss", forward_vol: "forward vol" };

/** The option filter in a line: only what is set. */
function optionFilterSummary(p) {
  const range = (k, label) => (p[`${k}Lo`] != null || p[`${k}Hi`] != null ? `${label} ${p[`${k}Lo`] ?? "…"}–${p[`${k}Hi`] ?? "…"}` : null);
  return [p.scrMinOi != null && `OI ≥ ${p.scrMinOi}`, p.scrMinVolume != null && `volume ≥ ${p.scrMinVolume}`,
    p.scrMinPrice != null && `price ≥ ${p.scrMinPrice}`, p.scrMaxSpreadPct != null && `spread ≤ ${p.scrMaxSpreadPct}`,
    range("scrDays", "days"), range("scrMoney", "K/F"), p.scrTwoSided && "two-sided"].filter(Boolean).join(" · ") || "none";
}

const FILTERS = [["scrDebit", "Debit"], ["scrCredit", "Credit"], ["scrLoss", "Max loss"], ["scrRr", "RR"], ["scrDelta", "Net Δ"],
  ["scrIv", "Avg IV"], ["scrEdge", "Edge", "model value − cost"], ["scrFwdVol", "Fwd vol"]];

/** The strategy filter (eight ranges): what is set, in the sidebar; all of it in a window. */
function StrategyFilter() {
  const [open, setOpen] = useState(false);
  const p = params.value;
  const set = FILTERS.flatMap(([k, label]) => [p[`${k}Lo`] != null && `${label} ≥ ${p[`${k}Lo`]}`, p[`${k}Hi`] != null && `${label} ≤ ${p[`${k}Hi`]}`]).filter(Boolean);
  const clear = () => FILTERS.forEach(([k]) => { setParam(`${k}Lo`, null); setParam(`${k}Hi`, null); });
  return (
    <>
      <Summary label="Strategy filter" action="Edit" onOpen={() => setOpen(true)}>
        {set.length ? <span class="flex flex-wrap gap-1">{set.map((t) => <span class="rounded-sm bg-accent-soft px-1.5 text-xs text-accent">{t}</span>)}</span>
          : <span class="text-xs text-muted">None: every strategy that passes the option filter</span>}
      </Summary>
      <Dialog open={open} onClose={() => setOpen(false)} title="Strategy filter" width="520px"
        footer={<><Button onClick={clear}>Clear all</Button><Button primary onClick={() => setOpen(false)}>Done</Button></>}>
        <div class="grid grid-cols-[96px_1fr_1fr] items-end gap-x-2 gap-y-2">
          <span /><span class="text-xs text-muted">at least</span><span class="text-xs text-muted">at most</span>
          {FILTERS.map(([k, label, hint]) => (
            <>
              <span class="pb-1 text-sm text-ink-2" title={hint}>{label}</span>
              <Num k={`${k}Lo`} label="" />
              <Num k={`${k}Hi`} label="" />
            </>
          ))}
        </div>
        <p class="mt-3 text-xs text-muted">Empty: no bound. Per contract, in USD; IV and forward vol as decimals.</p>
      </Dialog>
    </>
  );
}

function Params() {
  const p = params.value;
  return (
    <>
      <Group title="Data" action={<SmallButton onClick={loadSnapshots} title="Reload the list of cached chain snapshots">Snapshots ↻</SmallButton>}>
        <Seg k="scrView" label="View" options={[["strategies", "Strategies"], ["chain", "Option chain"]]} />
        <Seg k="scrCurrency" label="Underlying" options={underlyings.value.map((u) => [u, u])} wide={false} onChange={pickUnderlying} />
        <Select k="scrSnapshot" label={isDeribit(p.scrCurrency) ? "Chain" : "Day"} wide={false}
          options={[["", isDeribit(p.scrCurrency) ? "Live (Deribit)" : "Newest"], ...snapshots.value]} />
        {!isDeribit(p.scrCurrency) && p.scrSnapshot && (
          <Text k="scrAt" label="Time (New York)" placeholder="HH:MM, empty: the close" wide={false}
            hint="The quotes at or before this time on the chosen day" />
        )}
      </Group>
      {p.scrView === "strategies" && (
        <>
          <Group title="Strategies">
            <Chips items={[["scrSingle", "Call", "Single OTM call"], ["scrIc", "Iron condor"], ["scrStraddle", "Straddle"], ["scrStrangle", "Strangle"],
              ["scrFwdVol", "Calendar", "Same-strike calendar between two expiries, ranked by forward vol"]]} />
            <Seg k="scrDirection" label="Direction" options={[["LONG", "Long"], ["SHORT", "Short"]]}
              hint="LONG: buy premium (credit condor for IC, sell near / buy far for calendars). SHORT: the opposite." />
            <StrategyFilter />
          </Group>
          <DialogGroup title="Model and ranking" summary={`model ${MODEL_VOLS[p.scrModelVol] ?? p.scrModelVol}`
            + `${p.scrModelVol === "heston" ? (p.scrHestonFit ? " (fitted)" : " (given)") : ""} · ${p.scrPriceMode} prices · top ${p.scrTopN} by ${RANK_KEYS[p.scrRankKey] ?? p.scrRankKey}`}>
            <Seg k="scrModelVol" label="Model vol (edge = model value − cost)" options={[["mark", "Mark"],
              ...(isDeribit(p.scrCurrency) ? [["dvol", "DVOL"]] : []), ["surface", "Surface"], ["heston", "Heston"], ["none", "None"]]} />
            {p.scrModelVol === "heston" && <Toggle k="scrHestonFit" label="Calibrate Heston to the chain" wide />}
            {p.scrModelVol === "heston" && !p.scrHestonFit && (
              <>
                <Num k="scrHestonV0" label="v₀" step="0.01" /><Num k="scrHestonKappa" label="κ" step="0.1" />
                <Num k="scrHestonTheta" label="θ" step="0.01" /><Num k="scrHestonXi" label="ξ" step="0.05" />
                <Num k="scrHestonRho" label="ρ" step="0.05" wide />
              </>
            )}
            <Seg k="scrPriceMode" label="Prices" options={[["executable", "Executable", "Buy at the ask, sell at the bid"], ["mid", "Mid"]]} />
            <Num k="scrTopN" label="Top N" step="1" min="1" max="500" />
            <Seg k="scrRankKey" label="Rank by" options={[["rr", "RR"], ["edge", "Edge"], ["cost", "Cost"], ["credit", "Credit"], ["gain", "Gain"],
              ["loss", "Loss"], ["forward_vol", "Fwd vol"]]} />
          </DialogGroup>
        </>
      )}
      <DialogGroup title="Option filter" summary={optionFilterSummary(p)}>
        <Num k="scrMinOi" label="Min OI" hint="Open interest, in coins" />
        <Num k="scrMinVolume" label="Min volume" hint="24h volume, in coins" />
        <Num k="scrMinPrice" label="Min price" hint="Mid price, USD per coin" />
        <Num k="scrMaxSpreadPct" label="Max spread" hint="(ask − bid) / mid" step="0.01" />
        <Range k="scrDays" label="Days" />
        <Range k="scrMoney" label="K/F" hint="Strike / forward of the option's expiry" />
        <Toggle k="scrTwoSided" label="Two-sided only" hint="Drop options without both a bid and an ask" wide />
      </DialogGroup>

    </>
  );
}

const selected = signal(0);

async function run() {
  const p = params.value;
  selected.value = 0;
  if (p.scrView === "chain") {
    begin(ID, ["chain"]);
    const q = new URLSearchParams({ currency: p.scrCurrency });
    if (p.scrDaysHi > 0) q.set("max_days", String(p.scrDaysHi));
    if (p.scrSnapshot) q.set("snapshot_id", p.scrSnapshot);
    if (p.scrSnapshot && p.scrAt) q.set("at", p.scrAt);
    await fill(ID, "chain", get(`api/lab/screener/chain?${q}`));
    return;
  }
  begin(ID, ["strategies"]);
  await fill(ID, "strategies", post("api/lab/screener/run", P.screener(p)));
  loadSnapshots();  // a live run may have written a new snapshot
}

/** Sends a strategy to Multi-Leg or Risk (set by main.jsx: switches the tab and runs it). */
export const handOff = { to: null };

function source(s) {
  // a recorded or backtest day reads in New York time, as its quotes were taken; Deribit's in the browser's
  const ny = s.source === "backtest" || s.source === "recorded";
  const when = s.fetched_at ? new Date(s.fetched_at).toLocaleString(undefined, ny ? { timeZone: "America/New_York", timeZoneName: "short" } : {}) : "–";
  return <span><Badge kind={s.source === "live" ? "good" : "warn"}>{s.source || "?"}</Badge> <span class="text-muted">{when}</span></span>;
}

function Funnel({ s }) {
  const steps = [["Options", s.n_options_in], ["After option filter", s.n_options_after_filter], ["Combinations", s.n_generated],
    ["Passed filter", s.n_passed], ["Returned", s.n_returned]];
  const max = Math.log10(Math.max(...steps.map(([, v]) => Number(v) || 0), 10) + 1);
  return (
    <div class="flex flex-col gap-1">
      {steps.map(([label, v]) => (
        <div class="grid grid-cols-[120px_1fr_64px] items-center gap-2 text-xs">
          <span class="truncate text-ink-2">{label}</span>
          <span class="h-2 rounded-full bg-surface-2"><span class="block h-2 rounded-full bg-accent/70" style={{ width: `${Math.max(2, (Math.log10((Number(v) || 0) + 1) / max) * 100)}%` }} /></span>
          <span class="text-right tabular-nums">{Number(v ?? 0).toLocaleString()}</span>
        </div>
      ))}
      <Note>Log scale. By kind (passed / generated): {Object.entries(s.by_kind || {}).map(([k, c]) => `${k} ${Number(c.passed).toLocaleString()}/${Number(c.generated).toLocaleString()}`).join(" · ") || "–"}</Note>
    </div>
  );
}

const rr = (r) => (r.rr == null ? (r.max_gain_unbounded ? "∞" : "–") : Number(r.rr).toFixed(2));
const signedUsd = (v) => (v == null ? "–" : v >= 0 ? <Good>{usd(v)}</Good> : <Bad>{usd(v)}</Bad>);

function Strategies({ data }) {
  const s = data.result_summary || {}, rows = data.result_details?.strategies || [];
  const notes = data.diagnostics?.notes || [];
  const send = (r, where) => handOff.to?.(where, P.strategyPosition(r, s));
  return (
    <>
      <div class="grid gap-4 lg:grid-cols-[minmax(0,1fr)_minmax(0,1fr)]">
        <Kv cols={1} rows={[["Data", source(s)], ["Spot (index)", usd(s.spot)], ["Rate", s.rate],
          ["Model vol", s.model_vol_requested === s.model_vol ? s.model_vol : `${s.model_vol_requested} → ${s.model_vol}`],
          ["Prices", s.price_mode], ["Ranked by", `${s.rank_key} ${s.rank_descending ? "↓" : "↑"}`],
          s.heston_calibrated && ["Heston (fitted)", `κ ${fixed(s.heston_calibrated.kappa, 2)} · θ ${fixed(s.heston_calibrated.theta, 3)} · ξ ${fixed(s.heston_calibrated.xi, 2)} · ρ ${fixed(s.heston_calibrated.rho, 2)} · RMSE ${pct(s.heston_calibrated.rmse_rel, 2)}`],
          s.chain_quality && ["Quotes", `${pct(s.chain_quality.two_sided_share, 0)} two-sided · median spread ${pct(s.chain_quality.median_spread_pct, 1)}`]]} />
        <Funnel s={s} />
      </div>
      {notes.map((n) => <Note>{n}</Note>)}
      {rows.length ? (
        <Table compact maxHeight="420px" rows={rows} selected={selected.value} onRow={(_, i) => { selected.value = i; }} cols={[
          { key: "rank", label: "#", align: "left" }, { key: "label", label: "Strategy", align: "left", cls: "font-medium" },
          { key: "cost", label: "Cost", fmt: usd }, { key: "max_gain", label: "Max gain", fmt: (v, r) => bound(v, r.max_gain_unbounded) },
          { key: "max_loss", label: "Max loss", fmt: (v, r) => bound(v, r.max_loss_unbounded) }, { key: "rr", label: "RR", fmt: (_, r) => rr(r) },
          { key: "net_delta", label: "Δ", fmt: (v) => fixed(v, 3) }, { key: "net_theta", label: "Θ/day", fmt: usd },
          { key: "avg_iv", label: "IV", fmt: (v) => pct(v) }, { key: "edge", label: "Edge", fmt: signedUsd },
          { key: "send", label: "", fmt: (_, r) => (
            <span class="inline-flex gap-1" onClick={(e) => e.stopPropagation()}>
              <button type="button" class="rounded-sm border border-line px-1.5 text-xs text-ink-2 hover:border-accent hover:text-accent" onClick={() => send(r, "multileg")} title="Price in Multi-Leg">ML</button>
              <button type="button" class="rounded-sm border border-line px-1.5 text-xs text-ink-2 hover:border-accent hover:text-accent" onClick={() => send(r, "risk")} title="Stress in Risk">Risk</button>
            </span>) },
        ]} />
      ) : <Note>No strategy passed the filters.</Note>}
      <Note>USD per contract ({s.multiplier === 1 ? `1 ${s.currency}` : `${s.multiplier} × ${s.currency}`}). Edge = model value ({s.model_vol}) − cost. Click a row for its legs and payoff.</Note>
    </>
  );
}

// The expiry payoff of a single-expiry strategy, per contract, net of cost: piecewise linear between strikes.
function payoff(strategy, mult) {
  const legs = strategy.legs || [];
  if (!legs.length || new Set(legs.map((l) => l.expiry)).size > 1) return [];
  const ks = legs.map((l) => l.strike), f = legs[0].forward;
  const lo = Math.min(...ks, f) * 0.8, hi = Math.max(...ks, f) * 1.2;
  return [...new Set([lo, ...ks, hi])].sort((a, b) => a - b).map((x) => ({
    x, y: legs.reduce((a, l) => a + l.qty * mult * (l.option_type === "call" ? Math.max(x - l.strike, 0) : Math.max(l.strike - x, 0)), 0) - strategy.cost,
  }));
}
function breakevens(pts) {
  const out = [];
  for (let i = 1; i < pts.length; i++) {
    const a = pts[i - 1], b = pts[i];
    if (a.y < 0 !== b.y < 0 && b.y !== a.y) out.push(a.x + ((0 - a.y) * (b.x - a.x)) / (b.y - a.y));
  }
  return out;
}

function Detail({ data }) {
  const s = data.result_summary || {}, r = data.result_details?.strategies?.[selected.value];
  if (!r) return <Note>Pick a strategy.</Note>;
  const pts = payoff(r, Number(s.multiplier ?? 1)), be = breakevens(pts);
  return (
    <>
      <p class="text-sm font-semibold">{r.label}</p>
      <Kv rows={[["Cost", usd(r.cost)], ["Model value", usd(r.model_value)], ["Max gain", bound(r.max_gain, r.max_gain_unbounded)],
        ["Max loss", bound(r.max_loss, r.max_loss_unbounded)], ["Edge", signedUsd(r.edge)], ["Forward vol", r.forward_vol != null ? pct(r.forward_vol, 2) : "–"],
        ["Δ / Γ", `${fixed(r.net_delta)} / ${fixed(r.net_gamma, 6)}`], ["Θ / vega", `${usd(r.net_theta)} / ${usd(r.net_vega)}`]]} />
      <Table compact rows={r.legs || []} cols={[
        { key: "qty", label: "Leg", align: "left", fmt: (q, l) => `${q > 0 ? "Buy" : "Sell"} ${l.option_type.toUpperCase()}` },
        { key: "strike", label: "Strike" }, { key: "expiry", label: "Expiry", align: "left", fmt: (e, l) => `${e} (${(l.years * 365).toFixed(1)}d)` },
        { key: "fill_price", label: "Fill", fmt: usd }, { key: "mark", label: "Mark", fmt: usd }, { key: "iv", label: "IV", fmt: (v) => (v ? pct(v) : "–") },
        { key: "model_price", label: "Model", fmt: usd }, { key: "delta", label: "Δ", fmt: (v) => fixed(v) }, { key: "vega", label: "Vega", fmt: (v) => fixed(v, 2) },
      ]} />
      {pts.length ? (
        <LineChart height={180} series={[{ label: "P&L", points: pts, area: true, cls: "s3" }]} zero marks={[{ x: r.legs[0].forward, label: "F" }]}
          xFmt={(v) => `${Math.round(v / 1000)}k`} valueFmt={usd}
          caption={`P&L at expiry, net of cost · breakeven ${be.length ? be.map(usd).join(", ") : "none in range"}`} />
      ) : <Note>The legs span two expiries: the payoff depends on the far leg's value at the near expiry, so none is drawn.</Note>}
      <div class="flex gap-2">
        <button type="button" class="h-7 rounded-sm border border-line px-3 text-sm hover:border-accent hover:text-accent"
          onClick={() => handOff.to?.("multileg", P.strategyPosition(r, s))}>Price in Multi-Leg</button>
        <button type="button" class="h-7 rounded-sm border border-line px-3 text-sm hover:border-accent hover:text-accent"
          onClick={() => handOff.to?.("risk", P.strategyPosition(r, s))}>Stress in Risk</button>
      </div>
      {canBacktest(s) && <ComboBacktest key={`${s.fetched_at}-${selected.value}`} strategy={r} summary={s} />}
    </>
  );
}

function Chain({ data }) {
  const s = data.result_summary || {}, d = data.result_details || {};
  const expiries = d.expiries || [], chain = d.chain || [];
  const byExpiry = {};
  chain.forEach((o) => { (byExpiry[o.expiry] ||= []).push(o); });
  const listed = expiries.filter((e) => byExpiry[e.expiry]);
  const [picked, setExpiry] = useState("");
  const expiry = byExpiry[picked] ? picked : listed[0]?.expiry ?? "";  // a new run may not list the old pick
  const rows = byExpiry[expiry] || [];
  const F = rows[0]?.forward || 1;
  const strikes = [...new Set(rows.map((o) => o.strike))].sort((a, b) => a - b);
  const atm = strikes.reduce((b, k) => (Math.abs(k - F) < Math.abs(b - F) ? k : b), strikes[0]);
  const smile = rows.filter((o) => o.iv > 0 && ((o.option_type === "call" && o.strike >= F) || (o.option_type === "put" && o.strike < F)))
    .map((o) => ({ x: Math.log(o.strike / F), y: o.iv * 100, label: `K ${o.strike.toLocaleString()} ${o.option_type}` })).sort((a, b) => a.x - b.x);
  const side = (o, key) => (o?.[key] == null ? "–" : key === "iv" ? (o.iv ? pct(o.iv) : "–") : key === "oi" ? fmt(o.oi) : usd(o[key]));
  const table = strikes.map((k) => {
    const c = rows.find((o) => o.strike === k && o.option_type === "call"), p = rows.find((o) => o.strike === k && o.option_type === "put");
    return { k, cb: side(c, "bid"), ca: side(c, "ask"), civ: side(c, "iv"), coi: side(c, "oi"), pb: side(p, "bid"), pa: side(p, "ask"), piv: side(p, "iv"), poi: side(p, "oi") };
  });
  return (
    <>
      <Kv rows={[["Data", source(s)], ["Spot (index)", usd(s.spot)], ["Options", s.n_options], ["Expiries", s.n_expiries]]} />
      <Table compact maxHeight="220px" rows={expiries.map((e) => ({ ...e, days: e.years * 365 }))} onRow={(e) => byExpiry[e.expiry] && setExpiry(e.expiry)}
        selected={expiries.findIndex((e) => e.expiry === expiry)} cols={[
          { key: "expiry", label: "Expiry", align: "left" }, { key: "days", label: "Days", fmt: (v) => v.toFixed(1) }, { key: "forward", label: "Forward", fmt: usd },
          { key: "implied_carry", label: "Carry", fmt: (v) => pct(v, 2) }, { key: "n_options", label: "Options" }]} />
      <div class="grid gap-4 lg:grid-cols-[minmax(0,2fr)_minmax(0,3fr)]">
        <div class="min-w-0">
          <Sub>OTM smile · {expiry}</Sub>
          <LineChart height={200} series={[{ label: "IV", points: smile, dots: true }]} xLabel="ln(K/F)" marks={[{ x: 0, label: "F" }]}
            yFmt={(v) => `${(+v).toFixed(0)}%`} valueFmt={(v) => `${(+v).toFixed(1)}%`} caption="Mark IV: calls above the forward, puts below" />
        </div>
        <Table compact maxHeight="320px" rows={table} selected={strikes.indexOf(atm)} cols={[
          { key: "cb", label: "C bid" }, { key: "ca", label: "C ask" }, { key: "civ", label: "C IV" }, { key: "coi", label: "C OI" },
          { key: "k", label: "Strike", cls: "font-semibold", fmt: (v) => v.toLocaleString() },
          { key: "pb", label: "P bid" }, { key: "pa", label: "P ask" }, { key: "piv", label: "P IV" }, { key: "poi", label: "P OI" }]} />
      </div>
    </>
  );
}

function Results() {
  if (params.value.scrView === "chain") return <Slot mode={ID} slot="chain" title="Option chain" wide>{(d) => <Chain data={d} />}</Slot>;
  const ok = results.value[ID]?.strategies?.state === "ok";
  return (
    <>
      <Slot mode={ID} slot="strategies" title="Strategies" wide>{(d) => <Strategies data={d} />}</Slot>
      {ok && <Slot mode={ID} slot="strategies" title="Selected strategy" wide meta={null}>{(d) => <Detail data={d} />}</Slot>}
    </>
  );
}

const about = "Pull the live BTC or ETH option chain from Deribit, build calls, straddles, strangles, iron condors and calendars from it, and rank them by edge against a model volatility. Any strategy goes to Multi-Leg or Risk in one click.";

export default { id: ID, label: "Screener", about, Params, Results, run, precheck: false, enter: loadSnapshots };
