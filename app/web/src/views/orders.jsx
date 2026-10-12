// Orders and trades: what the live engine recorded in its database (SQLite), newest first.
import { signal } from "@preact/signals";
import { get } from "../lib/api.js";
import { count, fmt } from "../lib/format.js";
import { params } from "../lib/store.js";
import { Badge, Card, ErrorText, Running, Table } from "../ui/blocks.jsx";
import { Group, Line, Num, Seg, Select } from "../ui/controls.jsx";

const records = signal({ state: "idle", list: [], strategies: [] });
const busy = signal(false);

async function load() {
  const p = params.value;
  const query = new URLSearchParams({ limit: String(Math.min(5000, Math.max(1, p.otLimit ?? 200))) });
  if (p.otStrategy) query.set("strategy", p.otStrategy);
  if (p.otType !== "all") query.set("record_type", p.otType === "order" ? "Order" : "Trade");
  records.value = { ...records.value, state: "loading" };
  try {
    const d = await get(`api/orders-trades?${query}`);
    records.value = { state: "ok", list: d.records ?? [], strategies: d.strategies ?? records.value.strategies };
  } catch (err) {
    records.value = { state: "error", list: [], strategies: records.value.strategies, error: err.message };
  }
}

/** The Refresh button: the same load, with the button (and the tab) showing it. */
async function refresh() {
  busy.value = true;
  try {
    await load();
  } finally {
    busy.value = false;
  }
}

function Params() {
  return (
    <Group title="Filter">
      <Seg k="otType" label="Kind" options={[["all", "All"], ["order", "Orders"], ["trade", "Trades"]]} />
      <Select k="otStrategy" label="Strategy" placeholder="All strategies" options={records.value.strategies.map((s) => [s, s])} />
      <Num k="otLimit" label="Rows" min={1} max={5000} step={1} />
      <Line>From the live engine's database (data/optra.db), newest first.</Line>
    </Group>
  );
}

const SIDE = { LONG: "good", SHORT: "bad" };

function Results() {
  const r = records.value;
  if (r.state === "loading" && !r.list.length) return <Card title="Orders and trades" wide><Running /></Card>;
  return (
    <Card title="Orders and trades" wide meta={r.state === "ok" && `${count(r.list.length)} rows`}>
      {r.state === "error" ? <ErrorText>{r.error}</ErrorText> : (
        <Table compact rows={r.list} empty="Nothing recorded yet." cols={[
          { key: "timestamp", label: "Time", align: "left", cls: "text-muted" },
          { key: "record_type", label: "Kind", align: "left", fmt: (v) => <Badge kind={v === "Trade" ? "accent" : "muted"}>{v}</Badge> },
          { key: "strategy_name", label: "Strategy", align: "left" },
          { key: "id", label: "Id", align: "left", cls: "font-mono text-xs" },
          { key: "symbol", label: "Symbol", align: "left", cls: "font-mono text-xs" },
          { key: "direction", label: "Side", align: "left", fmt: (v) => (v ? <Badge kind={SIDE[v] ?? "muted"}>{v}</Badge> : "–") },
          { key: "price", label: "Price", fmt },
          { key: "volume", label: "Volume", fmt },
          { key: "traded", label: "Traded", fmt },
          { key: "status", label: "Status", align: "left" },
        ]} />
      )}
    </Card>
  );
}

export const orders = {
  id: "orders",
  label: "Orders & trades",
  about: "",
  enter: load,
  Params,
  Results,
  hasResults: () => true,
  status: () => "",
  action: { label: "Refresh", busyLabel: "Loading…", busy, run: refresh },
};
