// Risk: the stress library (a pack of scenarios at a severity) and the delta-hedge comparison.
import { signal } from "@preact/signals";
import { get, post } from "../lib/api.js";
import { fmt } from "../lib/format.js";
import * as P from "../lib/payloads.js";
import { begin, fail, fill, put } from "../lib/runner.js";
import { params } from "../lib/store.js";
import { Bad, Kv, Note, Slot, Sub, Table, Warn } from "../ui/blocks.jsx";
import { Group, Num, Seg, Toggle } from "../ui/controls.jsx";
import { DialogGroup } from "../ui/dialog.jsx";
import { LegsInput } from "./legs.jsx";
import { HBars, Histogram } from "../charts/Bars.jsx";
import { Frontier } from "../charts/Scatter.jsx";
import { MarketGroup, OptionGroup, PrecheckGroup } from "./shared.jsx";

const ID = "risk";
const packs = signal([["core4", "Core 4"], ["vol_first", "Vol first"], ["rates_first", "Rates first"], ["crash_kit", "Crash kit"]]);
let packsLoaded = false;
function loadPacks() {
  if (packsLoaded) return;
  packsLoaded = true;
  get("api/lab/stress/packs").then((d) => {
    const list = d.result_summary?.packs;
    if (Array.isArray(list) && list.length) packs.value = list.map((p) => [p.id, p.name, p.description]);
  }).catch(() => { packsLoaded = false; });
}

function Params() {
  const p = params.value;
  loadPacks();
  return (
    <>
      <Group title="Stress">
        <Seg k="stressPack" label="Scenario pack" options={packs.value} />
        <Seg k="stressSeverity" label="Severity" options={[["mild", "Mild"], ["moderate", "Moderate"], ["severe", "Severe"]]} />
        <Toggle k="stressIncludeHedge" label="Include hedge" />
        <LegsInput k="stressLegs" label="Portfolio (optional)" title="Portfolio to stress" empty="Empty: the single option (Market and Option below)" />
      </Group>
      <DialogGroup title="Delta hedging" summary={`${p.nReb} rebalances · ${Number(p.hedgePaths).toLocaleString()} paths · ${p.hedgeTcBps} bps · `
        + `threshold ${p.hedgeThreshold} · vol × ${p.hedgeVolMismatch}`}>
        <Num k="nReb" label="Rebalances" step="1" />
        <Num k="hedgePaths" label="Paths" step="500" />
        <Num k="hedgeTcBps" label="Cost (bps)" step="0.5" />
        <Num k="hedgeThreshold" label="Threshold" step="0.005" hint="Rebalance only when |target Δ − current Δ| exceeds this" />
        <Num k="hedgeVolMismatch" label="Vol mismatch ×" step="0.05" hint="The hedger's vol as a multiple of the true one" wide />
      </DialogGroup>
      <MarketGroup />
      <OptionGroup />
      <PrecheckGroup />
    </>
  );
}

async function run() {
  const p = params.value, b = P.base(p);
  const contract = { spot: b.spot, strike: b.strike, rate: b.rate, vol: b.vol, maturity: b.maturity, dividend_yield: b.dividend_yield };
  const hedge = P.hedge(p);
  let pos;
  begin(ID, ["stress", "hedge"]);
  try { pos = P.position(p.stressLegs); } catch (err) { fail(ID, "stress", err.message); return put(ID, "hedge", { skipped: "the portfolio JSON is invalid" }); }
  // A portfolio is stressed and hedged as a whole; otherwise the single option (Market and Option).
  const what = pos ? { spot: pos.spot ?? b.spot, rate: pos.rate ?? b.rate, legs: pos.legs } : { option_type: b.option_type };
  await Promise.all([
    fill(ID, "stress", post("api/lab/stress/run", { ...contract, ...what, ...hedge, n_paths: b.n_paths, hedge_paths: p.hedgePaths,
      stress_pack: p.stressPack, stress_severity: p.stressSeverity, include_hedge_compare: p.stressIncludeHedge })),
    fill(ID, "hedge", post("api/lab/hedging/run", { ...contract, ...what, ...hedge, n_paths: p.hedgePaths })),
  ]);
}

const ranking = (rows, key) => (rows || []).map((r) => ({ label: r.scenario, value: Number(r[key]) }));

function Stress({ data }) {
  const s = data.result_summary || {}, d = data.result_details || {}, c = d.cross_scenario_summary;
  const robust = c?.robustness_score;
  return (
    <>
      <Kv rows={[["Pack", s.pack], ["Severity", s.severity], ["Scenarios", s.scenario_count], ["Base MC", s.base_mc],
        ["Worst scenario", s.worst_scenario], ["Worst MC shift", s.worst_mc_shift != null ? <Bad>{fmt(s.worst_mc_shift)}</Bad> : "–"]]} />
      {c && (
        <>
          <Sub>Across scenarios</Sub>
          <Kv rows={[
            ["Robustness", robust == null ? "–" : <span class={robust >= 0.7 ? "text-up" : robust >= 0.4 ? "text-warn" : "text-down"}>{fmt(robust)}</span>],
            ["Key driver", c.key_driver], ["Driver share", c.key_driver_contribution_pct != null ? `${c.key_driver_contribution_pct}%` : "–"],
            ["Worst P&L", c.worst_pnl_impact != null ? <Bad>{fmt(c.worst_pnl_impact)}</Bad> : "–"],
            ["Breaching 5%", c.scenarios_breaching_threshold > 0 ? <Warn>{c.scenarios_breaching_threshold}</Warn> : c.scenarios_breaching_threshold ?? 0],
            ["ES95 mean · worst", `${fmt(c.hedge_resilience_mean)} · ${fmt(c.hedge_resilience_worst)}`],
          ]} />
        </>
      )}
      <Sub>MC shift against the base</Sub>
      <HBars signed rows={(d.tornado_points || []).map((r) => ({ label: r.name ?? r.label, value: r.value }))} />
      <div class="grid gap-4 sm:grid-cols-2">
        <div class="min-w-0"><Sub>Severity score</Sub><HBars rows={ranking(d.severity_ranking, "severity_score")} /></div>
        <div class="min-w-0"><Sub>Portfolio correlation</Sub><HBars rows={ranking(d.portfolio_correlation_ranking, "portfolio_correlation_score")} /></div>
        {d.hedge_resilience_ranking?.length > 0 && (
          <div class="min-w-0"><Sub>Hedge resilience (normalised std)</Sub><HBars rows={ranking(d.hedge_resilience_ranking, "hedge_normalized_std")} /></div>
        )}
      </div>
    </>
  );
}

function Hedge({ data }) {
  if (data.skipped) return <Note>{data.skipped}</Note>;
  const s = data.result_summary || data.summary || {}, d = data.result_details || data.details || {};
  const cmp = Object.entries(d.strategy_compare || {}).map(([name, x]) => ({ strategy: name, ...x }));
  const best = d.best_strategy || data.strategy_recommendation || {}, cfg = d.compare_config || {};
  return (
    <>
      <Kv rows={[["Strategy", s.strategy], ["Rebalances", s.n_rebalances], ["Paths", s.n_paths], ["P&L mean", s.pnl_mean], ["P&L std", s.pnl_std],
        ["Normalised std", s.normalized_std], ["q05 · q50 · q95", `${fmt(s.pnl_q05)} · ${fmt(s.pnl_q50)} · ${fmt(s.pnl_q95)}`], ["Tail skew", s.tail_skew_proxy]]} />
      <Histogram edges={d.histogram?.edges} counts={d.histogram?.counts} xLabel="hedged P&L"
        marks={[{ x: s.pnl_q05, label: "q05" }, { x: s.pnl_q50, label: "med" }, { x: s.pnl_q95, label: "q95" }]}
        caption={`P&L of ${s.n_legs > 1 ? `the ${s.n_legs}-leg position` : "one option sold"} at its Black-Scholes value, delta-hedged: ${s.n_paths ?? "?"} paths, ${s.n_rebalances ?? "?"} rebalances`} />
      {cmp.length > 0 && (
        <>
          <Sub>Strategies · cost {fmt(cfg.transaction_cost_bps)} bps · threshold {fmt(cfg.rebalance_threshold)} · vol ×{fmt(cfg.vol_mismatch_mult)}</Sub>
          <Frontier points={cmp.map((r) => ({ name: r.strategy, x: Number(r.transaction_cost ?? r.avg_cost), y: Number(r.es95 ?? r.var95) }))}
            xLabel="transaction cost" yLabel="ES95" caption="Green: the Pareto frontier (lower left is better)." />
          {best.name && <Note>Best: <b class="text-ink">{best.name}</b>{best.reason ? ` · ${best.reason}` : ""} · ES95 {fmt(best.es95)} · std {fmt(best.std)} · mean {fmt(best.mean)}</Note>}
          <Table compact rows={cmp} cols={[{ key: "strategy", label: "Strategy", align: "left" }, { key: "mean" }, { key: "std" }, { key: "q05" }, { key: "q50" },
            { key: "q95" }, { key: "var95", label: "VaR95" }, { key: "es95", label: "ES95" }, { key: "turnover" }, { key: "transaction_cost", label: "Cost" }]} />
        </>
      )}
    </>
  );
}

const Results = () => (
  <>
    <Slot mode={ID} slot="stress" title="Stress">{(d) => <Stress data={d} />}</Slot>
    <Slot mode={ID} slot="hedge" title="Delta hedging">{(d) => <Hedge data={d} />}</Slot>
  </>
);

const about = "Stress an option or a portfolio through a library of market shocks and see what drives the losses. Delta-hedge it four ways on the same simulated paths and see the frontier of hedging cost against tail risk.";

export default { id: ID, label: "Risk", about, Params, Results, run, precheck: true };
