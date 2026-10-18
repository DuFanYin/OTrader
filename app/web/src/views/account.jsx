// Account: the IB account (its values, its positions and what each is worth) beside the strategies' holdings,
// where the broker check stands, expired options waiting for a closing price to settle at, and a fill the engine
// missed booked by hand. Asked for on opening and on Refresh only: each ask is a subscription to IB's account window.
import { signal } from "@preact/signals";
import { get, post } from "../lib/api.js";
import { broker, gatewayName, live, refreshStatus } from "../lib/engine.js";
import { count, fixed, usd } from "../lib/format.js";
import { params, setParams } from "../lib/store.js";
import { Action, Badge, Card, ErrorText, Kv, Note, Running, Signed, Table } from "../ui/blocks.jsx";
import { Day, Group, Line, Num, Seg, Select, Text } from "../ui/controls.jsx";
import { DialogGroup } from "../ui/dialog.jsx";
import { toast } from "../ui/toast.jsx";

export const account = signal({ state: "idle" });  // { state: "ok", ...GET api/account } | { state: "error", error }
const checked = signal(null);                // the last check from here: { agree, mismatches, external }
const portfolios = signal([]);
const strategyNames = signal([]);
const busy = signal(false);
const settling = signal(false);
const booking = signal(false);

async function load() {
  await refreshStatus();
  if (!live.value.running) {
    account.value = { state: "idle" };
    return;
  }
  if (account.value.state !== "ok") account.value = { state: "loading" };
  try {
    const [a, p, s] = await Promise.all([get("api/account"), get("api/strategies/meta/portfolios").catch(() => ({})),
      get("api/strategies").catch(() => ({}))]);
    account.value = { state: "ok", ...a };
    portfolios.value = p.portfolios ?? [];
    strategyNames.value = (s.strategies ?? []).map((x) => x.strategy_name);
    if (!strategyNames.value.includes(params.value.acBookStrategy) && strategyNames.value.length) {
      setParams({ acBookStrategy: strategyNames.value[0] });
    }
    if (!portfolios.value.includes(params.value.acPortfolio) && portfolios.value.length) {
      setParams({ acPortfolio: portfolios.value[0] });
    }
  } catch (err) {
    account.value = { state: "error", error: err.message };
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

async function check() {
  try {
    checked.value = await post("api/account/check-with-broker");
  } catch (err) {
    toast(err.message, "Check positions");
  }
  await load();
}

async function settle() {
  const { acPortfolio: portfolio, acExpiry: expiry, acPrice: price } = params.value;
  if (!portfolio || !expiry || !(price > 0)) {
    toast("Give the portfolio, the expiry and the underlying's closing price.", "Settle expired options");
    return;
  }
  if (!confirm(`Settle ${portfolio}'s options that expired on ${expiry}, the underlying at ${price} at that close?`)) return;
  settling.value = true;
  try {
    const r = await post("api/account/settle", { portfolio, expiry, price });
    toast(`${count(r.settlements)} settlements booked.`, "Settle expired options");
    setParams({ acPrice: null });
  } catch (err) {
    toast(err.message, "Settle expired options");
  } finally {
    settling.value = false;
  }
  await load();
}

async function book() {
  const { acBookStrategy: strategy, acBookSymbol: symbol, acBookSide: side, acBookQuantity: quantity,
    acBookPrice: price, acBookReason: reason } = params.value;
  if (!strategy || !symbol.trim() || !(quantity > 0) || !(price >= 0) || !reason.trim()) {
    toast("Give the strategy, the contract, the side, the quantity, the price and why.", "Book a fill by hand");
    return;
  }
  if (!confirm(`Book for ${strategy}: ${side === "BUY" ? "bought" : "sold"} ${quantity} ${symbol.trim()} at ${price}? ` +
    `It is booked as a fill (saved, replayed on a restart) and changes the holding; it sends nothing to ${broker()}.`)) return;
  booking.value = true;
  try {
    const r = await post("api/account/book", { strategy, symbol: symbol.trim(), side, quantity, price, reason: reason.trim() });
    toast(`Booked as ${r.order_id}. Check with the broker again.`, "Book a fill by hand");
    setParams({ acBookQuantity: null, acBookPrice: null, acBookReason: "" });
  } catch (err) {
    toast(err.message, "Book a fill by hand");
  } finally {
    booking.value = false;
  }
  await load();
}

// ── inputs

function Params() {
  const p = params.value, running = live.value.running;
  return (
    <>
      <Group title="Show">
        <Seg k="acValues" label="Account values" options={[["main", "Main"], ["all", "All"]]} />
        <Seg k="acPositions" label="Positions" options={[["open", "Open"], ["all", "All today"]]} />
        <Line>From {broker()}'s account window through the connected gateway; refreshed when asked (Refresh), not polled.</Line>
      </Group>
      <DialogGroup title="Settle expired options" summary="Options held past a close the engine missed: close at intrinsic value or deliver" width="520px">
        <Line>Options held past a close the engine was not running at wait here for the underlying's closing price.
          The settlement is booked like a fill: closed at intrinsic value (cash), or delivering shares at the strike.</Line>
        <Select k="acPortfolio" label="Portfolio" wide={false} options={portfolios.value.map((x) => [x, x])}
          placeholder={portfolios.value.length ? undefined : "Engine not running"} />
        <Day k="acExpiry" label="Expiry" wide={false} />
        <Num k="acPrice" label="Underlying's close" min={0} hint="The underlying's price at that day's close (16:00 New York)" />
        <div class="col-span-full">
          <Action disabled={!running || settling.value || !p.acPortfolio || !p.acExpiry || !(p.acPrice > 0)} onClick={settle}>
            {settling.value ? "Settling…" : "Settle"}
          </Action>
        </div>
      </DialogGroup>
      <DialogGroup title="Book a fill by hand" summary={`A fill the engine missed while it was down, as ${broker()} has it`} width="520px">
        <Line>When the broker check disagrees and {broker()} is right (a fill the engine missed while it was down): booked as an
          order of the strategy's filled at once, saved and replayed on a restart. Nothing is sent to {broker()}.</Line>
        <Select k="acBookStrategy" label="Strategy" options={strategyNames.value.map((x) => [x, x])}
          placeholder={strategyNames.value.length ? undefined : "No strategies"} />
        <Text k="acBookSymbol" label="Contract" placeholder="SPY-20261013-C-660" hint="As the engine names it (the positions table)" />
        <Seg k="acBookSide" label="Side" options={[["BUY", "Bought"], ["SELL", "Sold"]]} />
        <Num k="acBookQuantity" label="Quantity" min={0} />
        <Num k="acBookPrice" label="Price" min={0} />
        <Text k="acBookReason" label="Why" placeholder="filled while the engine was down" />
        <div class="col-span-full">
          <Action disabled={!running || booking.value || !p.acBookStrategy} onClick={book}>
            {booking.value ? "Booking…" : "Book"}
          </Action>
        </div>
      </DialogGroup>
    </>
  );
}

// ── results

/** An account value by key: the account's base currency first, then USD. */
function value(values, key) {
  const of = values.filter((v) => v.key === key);
  return (of.find((v) => v.currency === "BASE") ?? of.find((v) => v.currency === "USD") ?? of[0])?.value;
}

const MAIN = [
  ["NetLiquidation", "Net liquidation"],
  ["TotalCashValue", "Cash"],
  ["SettledCash", "Settled cash"],
  ["AvailableFunds", "Available funds"],
  ["ExcessLiquidity", "Excess liquidity"],
  ["BuyingPower", "Buying power"],
  ["GrossPositionValue", "Gross position value"],
  ["InitMarginReq", "Initial margin"],
  ["MaintMarginReq", "Maintenance margin"],
  ["UnrealizedPnL", "Unrealised PnL", true],
  ["RealizedPnL", "Realised PnL", true],
];

function Summary() {
  const a = account.value, values = a.values ?? [];
  const num = (k) => { const v = value(values, k); return v == null || v === "" ? null : Number(v); };
  const cushion = num("Cushion");
  return (
    <Card title="Account" meta={a.from_ib && [a.account, a.updated && `as of ${a.updated}`].filter(Boolean).join(" · ")}>
      {!a.from_ib ? <ErrorText>{broker()}'s side is not available: {a.ib_error}</ErrorText> : (
        <Kv rows={[
          value(values, "AccountType") && ["Type", value(values, "AccountType")],
          ...MAIN.map(([k, label, signed]) => num(k) != null && [label, signed ? <Signed value={num(k)} format={usd} /> : usd(num(k))]),
          cushion != null && ["Cushion", `${fixed(cushion * 100, 1)}%`],
          ["Currency", value(values, "Currency") ?? values.find((v) => v.key === "NetLiquidation")?.currency],
        ]} />
      )}
    </Card>
  );
}

const CHECK = { agree: "good", disagree: "bad", failed: "bad", "not yet": "muted" };

function Check() {
  const a = account.value, c = checked.value, unsettled = Object.entries(a.unsettled ?? {});
  return (
    <Card title="Broker check" actions={<Action onClick={check} title={`Ask ${broker()} for the positions and check the holdings against them`}>Check again</Action>}>
      <Kv cols={1} rows={[
        [`Holdings against ${broker()}`, <Badge kind={CHECK[a.broker_check] ?? "muted"}>{a.broker_check || "–"}</Badge>],
        a.broker_check_problem && ["What differs", a.broker_check_problem],
      ]} />
      <Note>Checked on connecting and by Check again. A strategy starts only once the account's positions in the contracts
        the portfolios trade agree with the strategies' holdings summed (OPTRA_BROKER_CHECK=warn only logs a difference).</Note>
      {c && !c.agree && (
        <Table compact rows={c.mismatches} cols={[
          { key: "symbol", label: "Contract", align: "left", cls: "font-mono text-xs" },
          { key: "engine", label: "Strategies", fmt: count },
          { key: "broker", label: broker(), fmt: count },
        ]} />
      )}
      {unsettled.length > 0 && (
        <>
          <Table compact rows={unsettled.map(([strategy, options]) => ({ strategy, options: options.join(", ") }))} cols={[
            { key: "strategy", label: "Strategy", align: "left", cls: "font-medium" },
            { key: "options", label: "Expired, not settled", align: "left", cls: "font-mono text-xs whitespace-normal" },
          ]} />
          <Note>These strategies cannot start until their expired options are settled (in the sidebar).</Note>
        </>
      )}
    </Card>
  );
}

function Positions() {
  const a = account.value, open = params.value.acPositions !== "all";
  const rows = (a.positions ?? []).filter((p) => !open || p.quantity !== 0 || p.engine_quantity !== 0);
  const state = (p) => {
    if (!p.traded_here) return <Badge kind="muted">not traded here</Badge>;
    if (!a.from_ib) return <Badge kind="muted">unknown</Badge>;
    return p.mismatch ? <Badge kind="bad">differs</Badge> : <Badge kind="good">agrees</Badge>;
  };
  return (
    <Card title="Positions" wide meta={`${count(rows.length)} ${open ? "open" : "today"}`}>
      <Table compact rows={rows} empty={`No positions, at ${broker()} or in the strategies.`} cols={[
        { key: "symbol", label: "Contract", align: "left", cls: "font-mono text-xs" },
        { key: "quantity", label: broker(), fmt: (v) => (a.from_ib ? count(v) : "–") },
        { key: "engine_quantity", label: "Strategies", fmt: count },
        { key: "traded_here", label: "Check", align: "left", fmt: (_, p) => state(p) },
        { key: "average_cost", label: "Avg cost", fmt: (v) => (a.from_ib ? usd(v) : "–") },
        { key: "market_price", label: "Price", fmt: (v) => (a.from_ib ? usd(v) : "–") },
        { key: "market_value", label: "Value", fmt: (v) => (a.from_ib ? usd(v) : "–") },
        { key: "unrealized_pnl", label: "Unrealised", fmt: (v) => (a.from_ib ? <Signed value={v} format={usd} /> : "–") },
        { key: "realized_pnl", label: "Realised", fmt: (v) => (a.from_ib ? <Signed value={v} format={usd} /> : "–") },
      ]} />
    </Card>
  );
}

function AllValues() {
  const values = [...(account.value.values ?? [])].sort((x, y) => x.key.localeCompare(y.key) || x.currency.localeCompare(y.currency));
  return (
    <Card title="All account values" wide meta={`${count(values.length)} values`}>
      <Table compact maxHeight="480px" rows={values} cols={[
        { key: "key", label: "Key", align: "left", cls: "font-mono text-xs" },
        { key: "value", label: "Value", fmt: (v) => (v !== "" && Number.isFinite(Number(v)) ? usd(v) : v || "–") },
        { key: "currency", label: "Currency", align: "left", cls: "text-muted" },
      ]} />
    </Card>
  );
}

function Results() {
  const a = account.value;
  if (!live.value.running) {
    return (
      <Card title="Account" wide>
        <Note>The live engine is not running: the account comes from {broker()} through it (Live: connect the {gatewayName()}).</Note>
      </Card>
    );
  }
  if (a.state === "error") return <Card title="Account" wide><ErrorText>{a.error}</ErrorText></Card>;
  if (a.state !== "ok") return <Card title="Account" wide><Running /></Card>;
  return (
    <>
      <Summary />
      <Check />
      <Positions />
      {params.value.acValues === "all" && a.from_ib && <AllValues />}
    </>
  );
}

export const accountView = {
  id: "account",
  label: "Account",
  about: "",
  enter: load,
  Params,
  Results,
  hasResults: () => true,
  status: () => "",
  action: { label: "Refresh", get busyLabel() { return `Asking ${broker()}…`; }, busy, run: refresh },
};
