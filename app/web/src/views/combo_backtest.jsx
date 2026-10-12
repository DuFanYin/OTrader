// A strategy the screener built on a day the trading engine backtests or recorded, run as the engine's
// ComboStrategy over that underlying's days (the backtest_combo and hedge_study tools, in Optra's page
// only), and added to the paper account as it ran. The tools say everything; this shows it simply.
import { useState } from "preact/hooks";
import { post } from "../lib/api.js";
import { fixed, usd } from "../lib/format.js";
import { Bad, Good, Kv, Note, Sub, Table } from "../ui/blocks.jsx";

const BUTTON = "h-7 rounded-sm border border-line px-3 text-sm hover:border-accent hover:text-accent disabled:opacity-50";
const SOURCES = ["delta", "gamma", "theta", "vega", "other", "fills"];
const signed = (v) => (v == null ? "–" : v >= 0 ? <Good>{usd(v)}</Good> : <Bad>{usd(v)}</Bad>);

/** The New York day and minutes after the 09:30 open of the screener's quotes (ISO, New York offset). */
function when(fetchedAt) {
  const m = /^(\d{4}-\d{2}-\d{2})T(\d{2}):(\d{2})/.exec(fetchedAt || "");
  if (!m) return null;
  const minute = Number(m[2]) * 60 + Number(m[3]) - (9 * 60 + 30);
  return { day: m[1], minute: Math.min(Math.max(minute, 1), 370) };
}

export function canBacktest(summary) {
  return summary.source === "backtest" || summary.source === "recorded";
}

export function ComboBacktest({ strategy, summary }) {
  const [state, setState] = useState({});
  const at = when(summary.fetched_at);
  if (!at) return null;
  const body = {
    symbol: summary.currency, as_of: at.day, entry_minute: at.minute,
    legs: (strategy.legs || []).map((l) => ({ option_type: l.option_type, ratio: l.qty, strike: l.strike, expiry: l.expiry, forward: l.forward })),
  };
  const run = async (key, tool, extra = {}) => {
    setState((s) => ({ ...s, busy: key, error: null }));
    try {
      const d = await post(`api/tools/${tool}`, { ...body, ...extra });
      setState((s) => ({ ...s, busy: null, [key]: d }));
    } catch (err) {
      setState((s) => ({ ...s, busy: null, error: err.message }));
    }
  };
  const addToPaper = async () => {
    const spec = state.report?.result_details?.strategy;
    setState((s) => ({ ...s, busy: "paper", error: null }));
    try {
      const res = await fetch(new URL("api/strategies", document.baseURI), {
        method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(spec),
      });
      const d = await res.json();
      if (!res.ok || d.status === "error") throw new Error(d.detail || d.error || res.status);
      setState((s) => ({ ...s, busy: null, paper: d.strategy_name || d.name || "added" }));
    } catch (err) {
      setState((s) => ({ ...s, busy: null, error: `Not added: ${err.message}` }));
    }
  };
  const r = state.report?.result_summary, study = state.study?.result_details?.rows;
  return (
    <div class="flex flex-col gap-2 border-t border-line pt-3">
      <Sub>Backtest this combo as a rule</Sub>
      <Note>Each {summary.currency} day: the same legs by their distance from the forward and days to expiry, opened at minute {at.minute} (as here) and closed at 15:50.</Note>
      <div class="flex flex-wrap gap-2">
        <button type="button" class={BUTTON} disabled={!!state.busy} onClick={() => run("report", "backtest_combo")}>
          {state.busy === "report" ? "Backtesting…" : "Backtest over the days"}</button>
        <button type="button" class={BUTTON} disabled={!!state.busy} onClick={() => run("study", "hedge_study")}>
          {state.busy === "study" ? "Running…" : "Hedge study"}</button>
        {r && <button type="button" class={BUTTON} disabled={!!state.busy || !!state.paper} onClick={addToPaper}>
          {state.paper ? `Added: ${state.paper}` : "Add to paper"}</button>}
      </div>
      {state.error && <Note><Bad>{state.error}</Bad></Note>}
      {r && (
        <>
          <Kv rows={[["Days", r.days], ["Trades", r.trades], ["Win rate", r["win rate"]], ["Expectancy", `${r["expectancy R"]} R (t ${r.t})`],
            ["Net", r["net USD"]], ["Drawdown", r["drawdown USD"]], ["Fees", usd(r.fees)]]} />
          <Kv rows={SOURCES.map((k) => [`P&L ${k}`, signed(r.attribution?.[k])])} />
          {state.paper && <Note>Added to the paper account, not started: start it on the Live page.</Note>}
        </>
      )}
      {study && (
        <Table compact rows={study} cols={[
          { key: "variant", label: "Hedge", align: "left" }, { key: "mean", label: "Mean", fmt: usd }, { key: "std", label: "Std", fmt: usd },
          { key: "es95", label: "ES 95%", fmt: usd }, { key: "hedge_fills", label: "Share fills" }, { key: "fees", label: "Fees", fmt: usd },
          { key: "attribution", label: "Δ P&L", fmt: (a) => fixed(a?.delta, 0) }]} />
      )}
    </div>
  );
}
