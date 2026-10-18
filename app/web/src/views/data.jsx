// Data: the days there are to backtest on, and the live engine's database: its contracts, syncing them from IB,
// and clearing them or its record of orders and fills.
import { signal } from "@preact/signals";
import { get, post } from "../lib/api.js";
import { bytes, count } from "../lib/format.js";
import { contracts, files, loadContracts, loadFiles } from "../lib/data.js";
import { params, setParams, setSetting } from "../lib/store.js";
import { Action, Card, ErrorText, Running, Table } from "../ui/blocks.jsx";
import { Group, Line, Num, Select } from "../ui/controls.jsx";
import { toast } from "../ui/toast.jsx";
import { broker, gatewayName } from "../lib/engine.js";

const busy = signal(false);
const emptied = signal(null);  // what the last clear removed: { table: rows }
const portfolios = signal([]);  // the running engine's
const syncing = signal(false);
const synced = signal(null);  // the last sync: { portfolio, options, expiries }

const CLEAR = {
  trading: ["Clear orders and fills", "Delete every recorded order and fill, and the holdings saved from them? This cannot be undone."],
  contracts: ["Clear contracts", "Delete every contract saved? A running engine keeps the ones it trades; the next "
    + "sync (above, or the engine's own each morning) saves them again."],
};

async function sync() {
  const { syncPortfolio: portfolio, syncMaxDte } = params.value;
  syncing.value = true;
  try {
    const r = await post("api/contracts/sync", { portfolio, max_dte: syncMaxDte ?? 0 });
    synced.value = { portfolio, options: r.options, expiries: r.expiries };
    await loadContracts();
  } catch (err) {
    toast(err.message, "Sync contracts");
  } finally {
    syncing.value = false;
  }
}

async function loadPortfolios() {
  portfolios.value = (await get("api/strategies/meta/portfolios").catch(() => ({}))).portfolios ?? [];
  if (!portfolios.value.includes(params.value.syncPortfolio) && portfolios.value.length) {
    setParams({ syncPortfolio: portfolios.value[0] });
  }
}

async function clear(what) {
  if (!confirm(CLEAR[what][1])) return;
  try {
    emptied.value = (await post("api/database/wipe", { what })).emptied;
    await loadContracts();
  } catch (err) {
    toast(err.message, CLEAR[what][0]);
  }
}

async function load() {
  await Promise.all([loadFiles(), loadContracts(), loadPortfolios()]);
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
    <>
      <Group title="Backtest data">
        <Line>One Parquet file per trading day, under data/&lt;SYMBOL&gt;/&lt;SYMBOL&gt;-YYYY-MM/YYYYMMDD.parquet (see doc/data.md).</Line>
        <Line>No recorded data? POST /api/data/synthetic writes days from a model whose answers are known, to data/synthetic (out: "data" makes them the underlying's days).</Line>
      </Group>
      <Group title={`Contracts from ${broker()}`}>
        <Line>A portfolio's underlying and its options expiring within the days, from the connected {gatewayName()},
          rolled into the running engine at once, as it does itself each trading morning.</Line>
        <Select k="syncPortfolio" label="Portfolio" wide={false} options={portfolios.value.map((x) => [x, x])}
          placeholder={portfolios.value.length ? undefined : "Engine not running"} />
        <Num k="syncMaxDte" label="Days to expiry" wide={false} step={1} min={0} max={60} />
        <div class="col-span-full">
          <Action disabled={syncing.value || !params.value.syncPortfolio} onClick={sync}>
            {syncing.value ? "Syncing…" : "Sync contracts"}
          </Action>
        </div>
        {synced.value && (
          <Line>Rolled in {count(synced.value.options)} {synced.value.portfolio} options
            {synced.value.expiries.length ? `, expiring ${synced.value.expiries[0]} to ${synced.value.expiries.at(-1)}` : ""}:
            the engine trades them now.</Line>
        )}
      </Group>
      <Group title="Database">
        <Line>The live engine's contracts, orders and fills, in data/optra.db.</Line>
        <div class="col-span-full flex flex-wrap gap-2">
          {Object.entries(CLEAR).map(([what, [label]]) => <Action danger onClick={() => clear(what)}>{label}</Action>)}
        </div>
        {emptied.value && (
          <Line>Emptied {Object.entries(emptied.value).map(([table, rows]) => `${table} (${count(rows)})`).join(", ")}.</Line>
        )}
      </Group>
    </>
  );
}

function Symbols() {
  const f = files.value;
  // A symbol's row opens it in Backtest with its whole range.
  const open = (s) => {
    setParams({ btSymbol: s.name, btStart: s.date_start ?? "", btEnd: s.date_end ?? "" });
    setSetting("tab", "backtest");
  };
  return (
    <Card title="Backtest data" wide meta={f.state === "ok" && `${f.symbols.length} symbols`}>
      {f.state === "error" ? <ErrorText>{f.error}</ErrorText> : f.state === "loading" && !f.symbols.length ? <Running /> : (
        <Table rows={f.symbols} onRow={open} empty="No days under data/ yet." cols={[
          { key: "name", label: "Symbol", align: "left", cls: "font-medium" },
          { key: "date_start", label: "First day", align: "left" },
          { key: "date_end", label: "Last day", align: "left" },
          { key: "number_of_days", label: "Days", fmt: count },
          { key: "size_bytes", label: "Size", fmt: bytes },
        ]} />
      )}
    </Card>
  );
}

function Contracts() {
  const c = contracts.value;
  if (c.state === "error") return <Card title="Contracts" wide><ErrorText>{c.error}</ErrorText></Card>;
  if (!c.equity && !c.option) return <Card title="Contracts" wide><Running /></Card>;  // a reload keeps the last list on screen
  return (
    <>
      <Card title="Underlyings" meta={`${count(c.equity?.total)} contracts`}>
        <Table compact maxHeight="420px" rows={c.equity?.rows} empty="No underlyings in the database." cols={[
          { key: "symbol", label: "Symbol", align: "left", cls: "font-medium" },
          { key: "product", label: "Product", align: "left" },
          { key: "multiplier", label: "Multiplier" },
          { key: "price_tick", label: "Tick" },
        ]} />
      </Card>
      <Card title="Option chains" meta={`${count(c.option?.chains)} chains · ${count(c.option?.total)} options`}>
        <Table compact maxHeight="420px" rows={c.option?.rows} empty="No options in the database." cols={[
          { key: "chain", label: "Chain", align: "left", cls: "font-mono text-xs" },
          { key: "underlying", label: "Underlying", align: "left" },
          { key: "dte", label: "DTE" },
          { key: "calls", label: "Calls", fmt: count },
          { key: "puts", label: "Puts", fmt: count },
          { key: "strike_min", label: "Lowest strike" },
          { key: "strike_max", label: "Highest strike" },
        ]} />
      </Card>
    </>
  );
}

function Results() {
  return (
    <>
      <Symbols />
      <Contracts />
    </>
  );
}

export const data = {
  id: "data",
  label: "Data",
  about: "",
  enter: load,
  Params,
  Results,
  hasResults: () => true,
  status: () => "",
  action: { label: "Refresh", busyLabel: "Loading…", busy, run: refresh },
};
