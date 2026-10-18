// Backtest: a strategy over a range of recorded (or synthetic) days, one engine per day; the PnL path, the
// Greeks it carried and each day's result.
import { signal } from "@preact/signals";
import { LineChart } from "../charts/LineChart.jsx";
import { Columns } from "../charts/Columns.jsx";
import { post } from "../lib/api.js";
import { catalog, loadCatalog } from "../lib/catalog.js";
import { files, loadFiles } from "../lib/data.js";
import { bytes, count, fixed, isoDay, seconds, usd } from "../lib/format.js";
import { params, setParams } from "../lib/store.js";
import { Card, ErrorText, Kv, Note, Running, Signed, Table, Warnings } from "../ui/blocks.jsx";
import { Day, Group, Line, Num, Seg, Select } from "../ui/controls.jsx";
import { DialogGroup } from "../ui/dialog.jsx";
import { Costs, Parameters, changedOf, describe } from "./parameters.jsx";

// The last run: { state: "running" | "ok" | "error" | "cancelled", inputs, data, error, wall }
const last = signal(null);
const busy = signal(false);

const symbolOf = (name) => files.value.symbols.find((s) => s.name === name);

function chooseSymbol(name) {
  const s = symbolOf(name);
  setParams({ btSymbol: name, btStart: s?.date_start ?? "", btEnd: s?.date_end ?? "" });
}

async function enter() {
  if (files.value.state === "idle") await loadFiles();
  if (catalog.value.state === "idle") await loadCatalog();
  const p = params.value, first = files.value.symbols[0];
  if (first && !symbolOf(p.btSymbol)) chooseSymbol(first.name);
  if (!p.btStrategy && catalog.value.list.length) setParams({ btStrategy: catalog.value.list[0].name });
}

// Leaving the page mid-run stops the engine's process (it would otherwise run to the end for nobody).
const stopOnLeave = () => navigator.sendBeacon(new URL("api/backtest/cancel", document.baseURI).href);

async function run() {
  const p = params.value;
  if (!p.btSymbol || !p.btStrategy) throw new Error("Choose a symbol and a strategy first.");
  if (!p.btStart || !p.btEnd) throw new Error("Choose the first and the last day.");
  const changed = changedOf("btSettings", p.btStrategy);
  const costs = p.btCosts ?? {};
  const inputs = { symbol: p.btSymbol, strategy: p.btStrategy, start: p.btStart, end: p.btEnd, changed, costs,
    rate: p.btRiskFreeRate ?? 0, ivMode: p.btIvMode };
  busy.value = true;
  last.value = { state: "running", inputs };
  const started = performance.now();
  window.addEventListener("pagehide", stopOnLeave);
  try {
    const data = await post("api/run_backtest", {
      parquet: p.btSymbol, strategy: p.btStrategy, start_date: p.btStart, end_date: p.btEnd,
      costs, risk_free_rate: inputs.rate,
      iv_price_mode: p.btIvMode, strategy_setting: changed,
    });
    const wall = (performance.now() - started) / 1000;
    last.value = data.status === "cancelled" ? { state: "cancelled", inputs } : { state: "ok", inputs, data, wall };
  } catch (err) {
    last.value = { state: "error", inputs, error: err.message };
  } finally {
    window.removeEventListener("pagehide", stopOnLeave);
    busy.value = false;
  }
}

const stop = () => post("api/backtest/cancel");

// ── inputs

function Params() {
  const p = params.value, s = symbolOf(p.btSymbol);
  const symbols = files.value.symbols.map((x) => [x.name, x.name]);
  const strategies = catalog.value.list.map((x) => [x.name, x.name]);
  return (
    <>
      <Group title="Data">
        <Select label="Symbol" value={p.btSymbol} onChange={chooseSymbol} options={symbols}
          placeholder={files.value.state === "loading" ? "Loading…" : symbols.length ? undefined : "No data under data/"} />
        {s && <Line>{s.date_start} to {s.date_end} · {s.number_of_days} days · {bytes(s.size_bytes)}</Line>}
        <Day k="btStart" label="First day" min={s?.date_start} max={s?.date_end} />
        <Day k="btEnd" label="Last day" min={s?.date_start} max={s?.date_end} />
      </Group>
      <Group title="Strategy">
        <Select k="btStrategy" label="Class" options={strategies}
          placeholder={catalog.value.state === "error" ? "Engine not built" : strategies.length ? undefined : "Loading…"} />
        {catalog.value.state === "error" && <Line>{catalog.value.error}</Line>}
      </Group>
      <Parameters storeKey="btSettings" cls={p.btStrategy} />
      <Costs storeKey="btCosts" />
      <DialogGroup title="Pricing" summary={`r ${p.btRiskFreeRate} · IV from ${p.btIvMode}`}>
        <Num k="btRiskFreeRate" label="Risk-free rate" step="0.005" hint="For the Greeks (r in Black-Scholes); IV is undiscounted Black" />
        <Seg k="btIvMode" label="IV from" wide={false} options={[["mid", "Mid"], ["bid", "Bid"], ["ask", "Ask"]]} />
      </DialogGroup>
    </>
  );
}

// ── results

/** The day of each metric index, from the indices where a day starts (chart.day_boundaries). */
function dayIndex(boundaries, days) {
  return (x) => {
    let d = 0;
    while (d < boundaries.length && x >= boundaries[d]) d += 1;
    return { day: days[d] ?? "", bar: x - (d ? boundaries[d - 1] : 0) };
  };
}

function Charts({ data }) {
  const c = data.chart;
  if (!c?.pnl?.length) return null;
  const days = (data.days?.date ?? []).map(isoDay);
  const bounds = c.day_boundaries ?? [];
  const at = dayIndex(bounds, days);
  const series = (values) => values.map((y, i) => {
    const x = c.x_greek[i], { day, bar } = at(x);
    return { x, y, label: `${day} · bar ${bar + 1}` };
  });
  const dayTicks = [0, ...bounds].map((x, i) => ({ x, label: (days[i] ?? "").slice(5) }));
  const every = (n) => dayTicks.filter((_, i) => i % Math.ceil(dayTicks.length / n) === 0);
  const xTicks = every(12), fewTicks = every(4);  // a dozen days named on the big chart, four on the small ones
  const daily = bounds.length < 25;  // a line at each day's start while the days are few enough to tell apart
  const marks = daily ? bounds.map((x) => ({ x, label: "" })) : [];
  const greek = (label, values, cls) => (
    <div class="min-w-0">
      <p class="mb-1 text-xs text-muted">{label}</p>
      <LineChart height={160} series={[{ label, points: series(values), cls }]} xTicks={fewTicks} marks={marks} zero />
    </div>
  );
  return (
    <>
      <Card title="PnL" wide meta={`${count(c.pnl.length)} points`}>
        <LineChart height={240} series={[{ label: "PnL", points: series(c.pnl), area: true }]} xTicks={xTicks} marks={marks}
          zero valueFmt={usd} caption={`Cumulative PnL before fees, marked to mid${daily ? "; dashed lines start each day" : ""}.`} />
      </Card>
      <Card title="Greeks" wide>
        <div class="grid gap-3 md:grid-cols-3">
          {greek("Delta", c.delta, "s1")}
          {greek("Gamma", c.gamma, "s4")}
          {greek("Theta", c.theta, "s2")}
        </div>
      </Card>
    </>
  );
}

function Summary({ run }) {
  const r = run.data.summary ?? {}, f = run.data.day_files ?? {}, i = run.inputs;
  const total = (values) => (values ?? []).reduce((a, b) => a + b, 0);
  return (
    <Card title="Summary" wide meta={`${i.strategy} · ${i.symbol} ${i.start} to ${i.end}`}>
      <Kv cols={4} rows={[
        ["Net PnL", <Signed value={r.net} format={usd} />],
        ["Gross PnL", <Signed value={r.gross} format={usd} />],
        ["Fees", usd(r.fees)],
        ["Daily Sharpe", fixed(r.sharpe, 2)],
        ["Max drawdown", usd(r.drawdown)],
        ["Days", r.days],
        ["Orders", count(r.orders)],
        ["Bars", count(total(f.bars))],
        ["Max |delta|", fixed(r.max_delta, 2)],
        ["Max |gamma|", fixed(r.max_gamma, 4)],
        ["Max |theta|", fixed(r.max_theta, 2)],
        ["Option quotes", count(total(f.quotes))],
        ["Engine time", seconds(run.data.result?.duration_seconds)],
        ["Wall time", seconds(run.wall)],
      ]} />
      <Note>Parameters: {describe(i.changed)}. Costs: {describe(i.costs ?? {})}. IV from {i.ivMode}, r = {i.rate}.</Note>
      <Warnings items={run.data.errors} />
    </Card>
  );
}

/** The run's days as rows: its own (a column a field) with the bars and quotes of each day's file. */
function dayRows(data) {
  const d = data.days ?? {}, f = data.day_files ?? {};
  const at = Object.fromEntries((f.date ?? []).map((n, i) => [n, i]));
  return (d.date ?? []).map((n, i) => ({
    day: isoDay(n), pnl: d.pnl[i], fees: d.fees[i], net_pnl: d.net_pnl[i], orders: d.orders[i],
    timesteps: f.bars?.[at[n]], rows: f.quotes?.[at[n]],
  }));
}

function Daily({ data }) {
  const rows = dayRows(data);
  if (!rows.length) return null;
  return (
    <Card title="Days" wide meta={`${rows.length} days`}>
      {rows.length > 1 && <Columns values={rows.map((d) => d.net_pnl)} labels={rows.map((d) => d.day.slice(5))} fmt={usd} />}
      <Table compact maxHeight="360px" rows={rows} cols={[
        { key: "day", label: "Day", align: "left" },
        { key: "pnl", label: "Gross", fmt: (v) => <Signed value={v} format={usd} /> },
        { key: "fees", label: "Fees", fmt: usd },
        { key: "net_pnl", label: "Net", fmt: (v) => <Signed value={v} format={usd} /> },
        { key: "orders", label: "Orders", fmt: count },
        { key: "timesteps", label: "Bars", fmt: count },
        { key: "rows", label: "Quotes", fmt: count },
      ]} />
    </Card>
  );
}

function Results() {
  const run = last.value;
  if (run.state === "running") {
    return (
      <>
        <Card title="Summary" wide meta={`${run.inputs.strategy} · ${run.inputs.symbol}`}><Running /></Card>
        <Card title="PnL" wide><Running /></Card>
      </>
    );
  }
  if (run.state === "error") return <Card title="Backtest" wide><ErrorText>{run.error}</ErrorText></Card>;
  if (run.state === "cancelled") return <Card title="Backtest" wide><Note>Stopped before it finished.</Note></Card>;
  return (
    <>
      <Summary run={run} />
      <Charts data={run.data} />
      <Daily data={run.data} />
    </>
  );
}

export const backtest = {
  id: "backtest",
  label: "Backtest",
  about: "Run a strategy over recorded or synthetic option chains, one engine per day, with the code the live engine runs. "
    + "Days live under data/<SYMBOL>/; with none recorded, POST /api/data/synthetic with out: \"data\" writes days from a known model there.",
  enter,
  Params,
  Results,
  hasResults: () => last.value != null,
  status: () => (last.value?.wall != null ? `last run ${seconds(last.value.wall)}` : ""),
  action: { label: "Run backtest", busyLabel: "Running…", busy, run, stop },
  sample: { dump: () => last.value, show: (saved) => { last.value = saved; } },
};
