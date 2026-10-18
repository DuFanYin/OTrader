// Home: the live engine at a glance. The day in one row (the IB account, the strategies' P&L, positions, fills,
// refusals, the session check), the strategies it runs and what each holds, the latest fills, the risk limits every
// order is checked against (in a dialog) with the switch that halts trading, and the holdings under stress scenarios
// (on demand). Polled every few seconds while open; the fills and the check every 30 seconds.
import { signal } from "@preact/signals";
import { useEffect } from "preact/hooks";
import { del, follow, get, post } from "../lib/api.js";
import { catalog, loadCatalog } from "../lib/catalog.js";
import { broker, gatewayName, live, refreshStatus, status } from "../lib/engine.js";
import { count, fixed, usd } from "../lib/format.js";
import { poll } from "../lib/poll.js";
import { params, setParams } from "../lib/store.js";
import { Action, Badge, Card, ErrorText, Kv, Note, Signed, Sub, Table, Warnings } from "../ui/blocks.jsx";
import { Group, Line, Select, Text } from "../ui/controls.jsx";
import { Button, Dialog } from "../ui/dialog.jsx";
import { toast } from "../ui/toast.jsx";
import { account } from "./account.jsx";
import { Parameters, changedOf } from "./parameters.jsx";
import { WriteStrategy } from "./strategy_code.jsx";

const strategies = signal({ state: "idle", list: [] });  // [{ strategy_name, class_name, portfolio, status }]
const holdings = signal({});                              // strategy name -> { shares, options, summary }
const portfolios = signal([]);
const selected = signal(null);
const updated = signal(null);
const busy = signal(false);
const risk = signal(null);  // GET api/risk: { halted, halt_reason, halted_at, limits, refused, recent_refusals, day_stops }
const stressed = signal({ state: "idle" });  // the holdings_stress tool (GET api/risk/stress): its summary, details and warnings
const fills = signal({ state: "idle", list: [] });  // the latest fills the engine saved (GET api/orders-fills)
const check = signal(null);  // GET api/session/check: { problems }
const limitsOpen = signal(false);

/** Today in New York, as the engine's records are dated (YYYY-MM-DD). */
const today = () => new Date().toLocaleDateString("en-CA", { timeZone: "America/New_York" });

/** The fills and the session check: slower to change, asked every 30 seconds. */
async function refreshDay() {
  if (!live.value.running) return;
  const [f, c] = await Promise.all([get("api/orders-fills?record_type=Fill&limit=200").catch(() => null),
    get("api/session/check").catch(() => null)]);
  if (f) fills.value = { state: "ok", list: f.records ?? [] };
  if (c) check.value = c;
}

/** The strategies, their holdings and the risk state, as the server pushes them (api/live/stream) or refresh reads them. */
function show({ strategies: list, holdings: held, risk: r, error }) {
  if (error) {
    strategies.value = { ...strategies.value, state: "error", error };
    return;
  }
  const shown = list ?? [];
  strategies.value = { state: "ok", list: shown };
  holdings.value = held ?? {};
  risk.value = r ?? null;
  updated.value = new Date();
  if (!shown.some((x) => x.strategy_name === selected.value)) {  // a running one first
    selected.value = (shown.find((x) => x.status === "running") ?? shown[0])?.strategy_name ?? null;
  }
}

/** The same, read at once: on entering, after an action, and every 3 seconds when the stream cannot be had. */
async function refresh() {
  if (!live.value.running) return;
  try {
    const [s, h, r] = await Promise.all([get("api/strategies"), get("api/strategies/holdings"), get("api/risk").catch(() => null)]);
    show({ strategies: s.strategies, holdings: h.holdings, risk: r });
  } catch (err) {
    show({ error: err.message });
  }
}

async function enter() {
  await refreshStatus();
  if (catalog.value.state === "idle") await loadCatalog();
  const classes = catalog.value.list.map((x) => x.name);
  if (!classes.includes(params.value.liveClass) && classes.length) setParams({ liveClass: classes[0] });
  if (live.value.running) {
    portfolios.value = (await get("api/strategies/meta/portfolios").catch(() => ({}))).portfolios ?? [];
    if (!portfolios.value.includes(params.value.livePortfolio) && portfolios.value.length) {
      setParams({ livePortfolio: portfolios.value[0] });
    }
  }
  await Promise.all([refresh(), refreshDay()]);
  const held = Object.values(holdings.value).some((x) => Object.values(x.options ?? {}).some((o) => o.quantity));
  if (held && stressed.value.state === "idle") runStress();  // the stress card filled once, without a click
}

/** Run a request against the engine, report a failure, then show the engine's state. */
async function act(request, what) {
  try {
    await request();
  } catch (err) {
    toast(err.message, what);
  }
  await Promise.all([refreshStatus(), refresh()]);
}

async function addStrategy() {
  const p = params.value;
  if (!live.value.running) throw new Error("The live engine is not running.");
  if (!p.liveClass || !p.livePortfolio) throw new Error("Choose a strategy and a portfolio first.");
  busy.value = true;
  try {
    const r = await post("api/strategies", {
      strategy_class: p.liveClass, portfolio_name: p.livePortfolio,
      setting: changedOf("liveSettings", p.liveClass), strategy_name: p.liveName.trim() || null,
    });
    selected.value = r.strategy_name ?? selected.value;
    setParams({ liveName: "" });
    await refresh();
  } finally {
    busy.value = false;
  }
}

// ── inputs

/** A connection's state and the button that changes it. action: [label, onClick] */
function Connection({ name, on, state, action, disabled }) {
  return (
    <div class="col-span-full flex h-[26px] items-center justify-between gap-2 text-sm">
      <span class="flex min-w-0 items-center gap-2">
        <span class={`h-1.5 w-1.5 shrink-0 rounded-full ${on ? "bg-up" : "bg-line-strong"}`} />
        <span class="truncate">{name}</span>
        <span class="truncate text-xs text-muted">{state}</span>
      </span>
      <Action onClick={action[1]} disabled={disabled}>{action[0]}</Action>
    </div>
  );
}

const gateway = (to) => () => act(() => post(`api/gateway/${to}`), `${to === "connect" ? "Connect" : "Disconnect"} the ${gatewayName()}`);

function haltTrading() {
  const reason = prompt("Halt all trading: every strategy stops and every working order is cancelled; only closing orders go until the halt is cleared. Why?");
  if (reason === null) return;
  act(() => post("api/risk/halt", { reason: reason.trim() || "by the operator" }), "Halt trading");
}

function clearHalt() {
  if (!confirm("Clear the halt? Strategies may be started again (they stay stopped until started).")) return;
  act(() => post("api/risk/clear"), "Clear the halt");
}
const marketData = (to) => () => act(() => post(`api/market/${to}`), `${to === "start" ? "Start" : "Stop"} market data`);

function Params() {
  const p = params.value, l = live.value, off = !l.running;
  const classes = catalog.value.list.map((x) => [x.name, x.name]);
  return (
    <>
      <Group title="Connections">
        <Connection name={gatewayName()} on={l.ib} state={l.ib ? "connected" : "not connected"} disabled={off}
          action={l.ib ? ["Disconnect", gateway("disconnect")] : ["Connect", gateway("connect")]} />
        <Connection name="Market data" on={l.md} state={l.md ? "streaming" : "off"} disabled={off}
          action={l.md ? ["Stop", marketData("stop")] : ["Start", marketData("start")]} />
        {off && <Line>The live engine is not running.</Line>}
      </Group>
      <Group title="Trading">
        <Connection name="Orders" on={!l.halted} state={l.halted ? "halted: only closing orders" : "allowed"} disabled={off}
          action={l.halted ? ["Clear halt", clearHalt] : ["Halt all", haltTrading]} />
        <Line>Halt all stops every strategy, cancels every working order and refuses any order that does not only close a
          position, until cleared; a restart stays halted.</Line>
      </Group>
      <Group title="New strategy">
        <Select k="liveClass" label="Class" options={classes} placeholder={classes.length ? undefined : "Loading…"} />
        <Select k="livePortfolio" label="Portfolio" options={portfolios.value.map((x) => [x, x])}
          placeholder={portfolios.value.length ? undefined : "No portfolios"} />
        <Text k="liveName" label="Name (optional)" placeholder={p.liveClass && p.livePortfolio ? `${p.liveClass}_${p.livePortfolio}` : ""}
          hint="Name the instance to run one class more than once on a portfolio" />
      </Group>
      <Parameters storeKey="liveSettings" cls={p.liveClass} />
      <WriteStrategy />
    </>
  );
}

// ── results

const STATUS = { running: "good", error: "bad", inited: "muted", added: "accent" };

function Strategies() {
  const list = strategies.value.list, name = selected.value;
  const on = (path, what, method = post) => act(() => method(`api/strategies/${encodeURIComponent(name)}/${path}`), `${what} ${name}`);
  const pick = list.findIndex((s) => s.strategy_name === name);
  return (
    <Card title="Strategies" meta={updated.value && `updated ${updated.value.toLocaleTimeString()}`} actions={name && (
      <>
        <Action onClick={() => on("init", "Initialise")} title="on_init: choose chains, set up state">Init</Action>
        <Action onClick={() => on("start", "Start")} title="Start: on_timer runs every bar">Start</Action>
        <Action onClick={() => on("stop", "Stop")} title="Stop: on_stop (the built-in strategies close their positions)">Stop</Action>
        <Action danger onClick={() => { if (confirm(`Remove ${name}? It stops, and its holding is dropped.`)) on("remove", "Remove", del); }}>Remove</Action>
      </>
    )}>
      {strategies.value.state === "error" && <ErrorText>{strategies.value.error}</ErrorText>}
      <Table compact rows={list} selected={pick} onRow={(s) => (selected.value = s.strategy_name)}
        empty="No strategies yet: add one from the sidebar, then Init and Start it." cols={[
          { key: "strategy_name", label: "Name", align: "left", cls: "font-medium" },
          { key: "class_name", label: "Class", align: "left" },
          { key: "portfolio", label: "Portfolio", align: "left" },
          { key: "status", label: "Status", align: "left", fmt: (v) => <Badge kind={STATUS[v] ?? "muted"}>{v}</Badge> },
          { key: "strategy_name", label: "PnL", fmt: (v) => <Signed value={holdings.value[v]?.summary?.pnl} format={usd} /> },
          { key: "strategy_name", label: "Delta", fmt: (v) => fixed(holdings.value[v]?.summary?.delta, 2) },
        ]} />
    </Card>
  );
}

function Holding() {
  const name = selected.value, h = holdings.value[name];
  if (!name) return null;
  const s = h?.summary ?? {};
  // One row a contract, those opened together in one combo next to each other.
  const positions = Object.entries(h?.options ?? {}).map(([key, o]) => ({ key, ...o })).filter((o) => o.quantity !== 0)
    .sort((a, b) => (a.combo || a.key).localeCompare(b.combo || b.key) || a.key.localeCompare(b.key));
  const u = h?.shares;
  if (u?.quantity) positions.push({ key: u.symbol, ...u, combo_type: "shares" });
  return (
    <>
      <Card title={`Holding · ${name}`}>
        <Kv cols={2} rows={[
          ["PnL", <Signed value={s.pnl} format={usd} />],
          ["Unrealised", <Signed value={s.unrealized_pnl} format={usd} />],
          ["Realised", <Signed value={s.realized_pnl} format={usd} />],
          ["Value", usd(s.current_value)],
          ["Cost", usd(s.total_cost)],
          ["Delta", fixed(s.delta, 2)],
          ["Gamma", fixed(s.gamma, 4)],
          ["Theta", fixed(s.theta, 2)],
          ["Vega", fixed(s.vega, 2)],
        ]} />
        <Sub>P&L by source since the engine started</Sub>
        <Kv cols={3} rows={["delta", "gamma", "theta", "vega", "other", "fills"].map((k) => [k, <Signed value={s[`pnl_${k}`]} format={usd} />])} />
      </Card>
      <div class="flex min-w-0 flex-col gap-3">
        <Card title="Positions" meta={`${positions.length} open`}>
          <Table compact rows={positions} empty="No open positions." cols={[
            { key: "key", label: "Position", align: "left", cls: "font-mono text-xs" },
            { key: "combo_type", label: "Opened in", align: "left", cls: "max-w-[11rem] truncate", fmt: (v, r) => (v === "shares" ? v : r.combo && r.combo !== r.key ? `${(v || "custom").toLowerCase()} ${r.combo}` : "single") },
            { key: "quantity", label: "Qty", fmt: count },
            { key: "avg_cost", label: "Avg cost", fmt: usd },
            { key: "mid_price", label: "Mid", fmt: usd },
            { key: "delta", label: "Delta", fmt: (v) => fixed(v, 2) },
          ]} />
        </Card>
        <Fills />
      </div>
    </>
  );
}

/** The halt, the limits every order is checked against (a row clicked: a new value), and the latest refusals. */
function Risk() {
  const r = risk.value;
  if (!r) return null;
  const edit = (limit) => {
    const text = prompt(`${limit.name}: from ${limit.min} to ${limit.max}${limit.integer ? ", a whole number" : ""} (default ${limit.default})`, String(limit.value));
    if (text === null || text.trim() === "") return;
    const value = Number(text);
    if (!Number.isFinite(value)) return toast(`${text} is not a number.`, "Risk limit");
    act(() => post("api/risk/limits", { values: { [limit.name]: value } }), `Set ${limit.name}`);
  };
  const changed = r.limits.filter((l) => l.value !== l.default), inForce = r.limits.filter((l) => l.value !== 0);
  return (
    <Card title="Risk" meta={`${count(r.refused)} refused since the engine started`}
      actions={<Action onClick={() => (limitsOpen.value = true)} title="Every limit, its default and its range">Limits</Action>}>
      {r.halted ? <ErrorText>Trading halted{r.halted_at ? ` at ${r.halted_at} UTC` : ""}: {r.halt_reason}. Only closing orders go; clear the halt in the sidebar.</ErrorText>
        : <Note>Orders allowed. The limits in force ({inForce.length} of {r.limits.length}; 0 is off, Limits shows every one){changed.length ? `, ${changed.length} set apart from the default (amber)` : ""}.</Note>}
      {Object.values(r.day_stops ?? {}).map((why) => <ErrorText key={why}>{why}: only closing orders until tomorrow.</ErrorText>)}
      <Kv rows={inForce.map((l) => [l.name, <span class={l.value !== l.default ? "text-warn" : ""} title={`default ${l.default}`}>{fixed(l.value, l.integer ? 0 : 2)}</span>])} />
      {r.recent_refusals.length > 0 && (
        <Table compact maxHeight="10rem" rows={[...r.recent_refusals].reverse().map((line) => ({ line }))} cols={[
          { key: "line", label: "Latest refusals", align: "left", cls: "font-mono text-xs whitespace-normal" },
        ]} />
      )}
      <Dialog open={limitsOpen.value} onClose={() => (limitsOpen.value = false)} title="Risk limits" width="680px"
        footer={<Button primary onClick={() => (limitsOpen.value = false)}>Done</Button>}>
        <Table compact rows={r.limits} onRow={edit} cols={[
          { key: "name", label: "Limit (click to change)", align: "left", cls: "font-mono text-xs" },
          { key: "value", label: "Value", fmt: (v) => fixed(v, 2) },
          { key: "default", label: "Default", fmt: (v) => fixed(v, 2) },
          { key: "max", label: "Range", fmt: (v, row) => `${row.min} – ${row.max}` },
        ]} />
      </Dialog>
    </Card>
  );
}

/** An IB account value, in the account's base currency where IB gives one (P&L comes as $LEDGER-<key>). */
function accountValue(key) {
  const values = account.value.values ?? [];
  const find = (k) => values.find((x) => x.key === k && x.currency === "BASE") ?? values.find((x) => x.key === k);
  const v = find(key) ?? find(`$LEDGER-${key}`);
  return v ? { value: Number(v.value), currency: v.currency === "BASE" ? "" : v.currency } : null;
}

/** The day in one row. */
function Today() {
  const list = strategies.value.list, h = holdings.value, c = check.value;
  const pnl = Object.values(h).reduce((sum, x) => sum + (x.summary?.pnl ?? 0), 0);
  const running = list.filter((x) => x.status === "running").length;
  const positions = Object.values(h).reduce((n, x) => n + Object.values(x.options ?? {}).filter((o) => o.quantity).length, 0);
  const day = today(), todays = fills.value.list.filter((f) => String(f.timestamp).startsWith(day));
  const nl = accountValue("NetLiquidation"), upnl = accountValue("UnrealizedPnL"), rpnl = accountValue("RealizedPnL");
  const money = (x) => (x ? <Signed value={x.value} format={(v) => `${usd(v)}${x.currency ? ` ${x.currency}` : ""}`} /> : "–");
  return (
    <Card title="Today" meta={`${day} New York`}>
      <Kv cols={2} rows={[
        ["Net liquidation", nl ? `${usd(nl.value)}${nl.currency ? ` ${nl.currency}` : ""}` : "–"],
        ["Strategies' P&L", <Signed value={pnl} format={usd} />],
        [`${broker()} unrealised`, money(upnl)],
        [`${broker()} realised`, money(rpnl)],
        ["Strategies running", `${running} of ${list.length}`],
        ["Option positions", count(positions)],
        ["Fills today", count(todays.length)],
        ["Orders refused", count(risk.value?.refused)],
        ["Session check", !c ? "–" : c.problems?.length ? <span class="text-down">{c.problems.length} problem{c.problems.length > 1 ? "s" : ""}</span> : <span class="text-up">all good</span>],
        ["Trading", risk.value?.halted ? <span class="text-down">halted</span> : "allowed"],
      ]} />
      {c?.problems?.length > 0 && <Warnings items={c.problems} />}
    </Card>
  );
}

/** The latest fills, newest first. */
function Fills() {
  const list = fills.value.list.slice(0, 8);
  return (
    <Card title="Latest fills" meta={fills.value.state === "ok" ? `${count(fills.value.list.length)} kept` : ""}>
      <Table compact rows={list} empty="No fills yet." cols={[
        { key: "timestamp", label: "Time", align: "left", fmt: (v) => String(v).slice(5, 16) },
        { key: "strategy_name", label: "Strategy", align: "left", cls: "max-w-[12rem] truncate" },
        { key: "symbol", label: "Contract", align: "left", cls: "max-w-[14rem] truncate font-mono text-xs" },
        { key: "side", label: "Side", align: "left" },
        { key: "quantity", label: "Qty", fmt: count },
        { key: "price", label: "Price", fmt: (v) => fixed(v, 2) },
      ]} />
    </Card>
  );
}

async function runStress() {
  stressed.value = { ...stressed.value, state: "busy" };
  try {
    const d = await get("api/risk/stress");
    stressed.value = { state: "ok", ...d.result_summary, ...d.result_details, warnings: d.warnings };
  } catch (err) {
    stressed.value = { state: "error", error: err.message };
  }
}

/** Every strategy's holding under the lab's stress scenarios and a spot ladder: P&L in USD, on the newest quotes. */
function Stress() {
  const r = stressed.value;
  const names = r.state === "ok" ? r.strategies.map((s) => s.strategy) : [];
  const rows = r.state === "ok" ? r.scenarios.map((sc) => ({
    scenario: sc.name, description: sc.description, total: r.total[sc.name],
    ...Object.fromEntries(r.strategies.map((s) => [s.strategy, s.pnl[sc.name]])),
  })) : [];
  const marked = r.state === "ok" ? Object.entries(r.marked_at).map(([root, at]) => `${root} at ${at.slice(0, 16).replace("T", " ")} NY (${r.marked_on[root]})`).join(", ") : "";
  return (
    <Card title="Stress" meta={marked}
      actions={<Action onClick={runStress} disabled={r.state === "busy"} title="Value every holding again under each scenario">
        {r.state === "busy" ? "Running…" : r.state === "idle" ? "Run" : "Run again"}</Action>}>
      {r.state === "idle" && <Note>The holdings revalued under the lab&apos;s stress pack (core4) and a spot ladder: each option at its own implied vol and its expiry&apos;s forward from the newest quotes, by the engine&apos;s Black.</Note>}
      {r.state === "error" && <ErrorText>{r.error}</ErrorText>}
      {r.state === "ok" && !names.length && <Note>No holdings to stress.</Note>}
      {r.state === "ok" && names.length > 0 && (
        <>
          <Table compact rows={rows} cols={[
            { key: "scenario", label: "Scenario", align: "left", fmt: (v, row) => <span title={row.description}>{v}</span> },
            { key: "total", label: "Total", fmt: (v) => <Signed value={v} format={usd} /> },
            ...names.map((n) => ({ key: n, label: n, fmt: (v) => <Signed value={v} format={usd} /> })),
          ]} />
          <Warnings items={[...r.warnings, ...r.unpriced.map((u) => `not valued: ${u}`)]} />
        </>
      )}
    </Card>
  );
}

function Results() {
  useEffect(() => {
    const down = () => status.value.state === "down";
    let polling = null;
    const stops = [
      follow("api/live/stream", { onData: show, onUnchanged: () => { updated.value = new Date(); },
        failed: () => { polling ??= poll(refresh, 3000, down); } }),
      () => polling?.(),
      poll(refreshDay, 30000, down),
    ];
    return () => stops.forEach((stop) => stop());
  }, []);
  if (status.value.state === "down") return <Card title="Live" wide><ErrorText>The backend is not answering: {status.value.error}</ErrorText></Card>;
  if (!live.value.running) {
    return (
      <Card title="Live engine" wide>
        <Note>The live engine (engine/build/entry_live) is not running, or the backend cannot reach it on its gRPC port.
          ./scripts/dev.sh starts it with the backend and this page; it loads its contracts from data/optra.db and
          takes orders and quotes through IB Gateway (IB_HOST, IB_PORT).</Note>
        {live.value.detail && <Note>Engine: {live.value.detail}</Note>}
      </Card>
    );
  }
  // Two columns: the day beside the strategies, the chosen one's holding beside its positions and the fills, the
  // risk beside the stress.
  return (
    <>
      <Today />
      <Strategies />
      {selected.value ? <Holding /> : <Fills />}
      <Risk />
      <Stress />
    </>
  );
}

export const home = {
  id: "home",
  label: "Home",
  about: "",
  enter,
  Params,
  Results,
  hasResults: () => true,
  status: () => "",
  action: { label: "Add strategy", busyLabel: "Adding…", busy, run: addStrategy },
};
