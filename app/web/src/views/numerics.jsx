// Numerics: the PDE solver, lattice convergence, the method benchmark, and the change of measure.
import { post } from "../lib/api.js";
import { fmt } from "../lib/format.js";
import * as P from "../lib/payloads.js";
import { begin, fill } from "../lib/runner.js";
import { params } from "../lib/store.js";
import { Good, Kv, Slot, Sub, Warn, tone } from "../ui/blocks.jsx";
import { Num, Seg, Text } from "../ui/controls.jsx";
import { DialogGroup } from "../ui/dialog.jsx";
import { Multiples } from "../charts/Bars.jsx";
import { LineChart } from "../charts/LineChart.jsx";
import { MarketGroup, OptionGroup, PrecheckGroup } from "./shared.jsx";

const ID = "numerics";

const SCHEMES = [["crank_nicolson", "Crank-Nicolson"], ["implicit", "Implicit"],
  ["explicit", "Explicit", "The time grid is refined automatically when the stability limit is broken"]];
const BASELINES = [["pde", "PDE"], ["bs", "BS"], ["mc", "MC"], ["binomial", "Binomial"]];
const nameOf = (options, v) => options.find(([o]) => o === v)?.[1] ?? v;

function Params() {
  const p = params.value, rn = p.measureMode === "rn";
  return (
    <>
      <DialogGroup title="PDE" summary={`${p.pdeSSteps} × ${p.pdeTSteps} grid · ${nameOf(SCHEMES, p.pdeMethod)}`}>
        <Num k="pdeSSteps" label="S steps" step="20" />
        <Num k="pdeTSteps" label="T steps" step="20" />
        <Seg k="pdeMethod" label="Scheme" options={SCHEMES} />
      </DialogGroup>
      <DialogGroup title="Convergence and benchmark" summary={`steps ${p.convSteps} · against ${nameOf(BASELINES, p.benchmarkBase)}`}>
        <Text k="convSteps" label="Step ladder" />
        <Seg k="benchmarkBase" label="Baseline" options={BASELINES} />
      </DialogGroup>
      <DialogGroup title="Measure" summary={rn ? `RN density · ${p.measureN} steps` : `P vs Q · ${p.cmpSteps} steps · ${Number(p.cmpPaths).toLocaleString()} paths`}>
        <Seg k="measureMode" label="Show" options={[["pq", "P vs Q"], ["rn", "RN density"]]} />
        {rn ? <Num k="measureN" label="Steps" step="50" wide /> : <><Num k="cmpSteps" label="Steps" step="50" /><Num k="cmpPaths" label="Paths" step="1000" /></>}
      </DialogGroup>
      <MarketGroup drift />
      <OptionGroup />
      <PrecheckGroup />
    </>
  );
}

async function run() {
  const p = params.value, b = P.base(p);
  const contract = { spot: b.spot, strike: b.strike, rate: b.rate, vol: b.vol, maturity: b.maturity, dividend_yield: b.dividend_yield, option_type: b.option_type };
  const measure = { mu: p.mu, r: p.rate, sigma: p.vol, t: p.maturity };
  begin(ID, ["pde", "convergence", "benchmark", "measure"]);
  await Promise.all([
    fill(ID, "pde", post("api/lab/pde/run", { ...contract, s_steps: p.pdeSSteps, t_steps: p.pdeTSteps, method: p.pdeMethod, is_american: b.is_american })),
    fill(ID, "convergence", post("api/lab/convergence/run", { ...contract, step_ladder: P.ladder(p) })),
    fill(ID, "benchmark", post("api/lab/benchmark/run", { pricing: b, pde_method: p.pdeMethod, pde_s_steps: p.pdeSSteps, pde_t_steps: p.pdeTSteps,
      benchmark_baseline: p.benchmarkBase })),
    fill(ID, "measure", (p.measureMode === "rn"
      ? post("api/lab/measure/run", { ...measure, n_steps: p.measureN })
      : post("api/lab/measure/compare", { ...measure, n_steps: p.cmpSteps, n_paths: p.cmpPaths, x0: 1.0 })).then((d) => ({ ...d, kind: p.measureMode }))),
  ]);
}

function Pde({ data }) {
  const s = data.result_summary || {};
  return (
    <>
      <div class="flex flex-wrap items-baseline gap-x-4">
        <span class="text-2xl font-semibold tracking-tight">{fmt(s.price)}</span>
        <span class="text-sm text-muted">{s.is_american ? "American" : "European"} {s.option_type || "call"} · {s.method}</span>
      </div>
      <Kv rows={[
        ["Reference", s.reference_method ? `${s.reference_method} ${fmt(s.reference_price)}` : "–"],
        ["Gap to reference", tone(s.price_vs_bs_gap, Math.abs(s.price_vs_bs_gap) < 0.001)],
        ["S grid", s.s_steps], ["T grid", s.stability_refined ? <Warn>{s.t_steps} → {s.t_steps_used} (stability)</Warn> : s.t_steps],
        ["PSOR sweeps", s.is_american && s.method !== "explicit" ? s.psor_iterations : "–"], ["Grid points", s.grid_points],
        ["S/T aspect", s.s_t_aspect_ratio], ["Density per maturity", s.grid_density_per_maturity],
      ]} />
    </>
  );
}

function Convergence({ data }) {
  const s = data.result_summary || {}, d = data.result_details || {}, tri = d.trinomial_curve_points || [];
  const series = [{ label: "Binomial", points: (d.curve_points || []).map((p) => ({ x: p.x, y: p.y })), dots: true }];
  if (tri.length) series.push({ label: "Trinomial", points: tri.map((p) => ({ x: p.x, y: p.y })), dots: true, cls: "s2" });
  return (
    <>
      <Kv rows={[["BS reference", s.bs_ref], ["Best |error|", s.best_abs_error != null ? <Good>{fmt(s.best_abs_error)}</Good> : "–"],
        ["Worst |error|", s.worst_abs_error], ["Best steps", s.best_steps], ["Last error", s.last_error], ["Log slope", s.log_slope],
        ["Improvement", tone(s.improvement_ratio, s.improvement_ratio > 2)],
        ["Monotonicity breaks", s.monotonicity_break_count > 0 ? <Warn>{s.monotonicity_break_count}</Warn> : s.monotonicity_break_count ?? 0],
        s.trinomial && ["Trinomial best / slope", `${fmt(s.trinomial.best_abs_error)} / ${fmt(s.trinomial.log_slope)}`]]} />
      <LineChart height={190} series={series} xLabel="steps" caption="|price − BS| against the number of lattice steps" />
    </>
  );
}

function Benchmark({ data }) {
  const s = data.result_summary || {}, d = data.result_details || {};
  return (
    <>
      <Kv rows={[["Baseline", `${s.baseline_method ?? "–"} ${fmt(s.baseline_price)}`], ["Winner", s.winner_method ? <Good>{s.winner_method}</Good> : "–"],
        ["Winner rel. error", tone(s.winner_rel_error, s.winner_rel_error < 0.001)]]} />
      <Multiples rows={d.chart_rows || d.rows || []} labelKey="method" keys={["price", "runtime_ms", "accuracy_abs_error"]} />
    </>
  );
}

function Measure({ data }) {
  const s = data.result_summary || {}, d = data.result_details || {};
  if (data.kind === "rn") {
    return (
      <>
        <Kv rows={[["θ market price of risk", s.theta_market_price_of_risk], ["Terminal density", s.final_density], ["Density CV", s.density_cv],
          ["Autocorrelation (lag 1)", s.density_autocorr_lag1]]} />
        <LineChart height={170} series={[{ label: "density", points: (d.density_preview || []).map((y, i) => ({ x: i, y })), area: true }]}
          xLabel="step" caption="Radon-Nikodym density dQ/dP along the path (first 30 steps)" />
      </>
    );
  }
  const P = d.path_preview?.P || [], Q = d.path_preview?.Q || [];
  return (
    <>
      <Kv rows={[["θ market price of risk", s.theta_market_price_of_risk], ["Drift P / Q", `${fmt(s.drift_p)} / ${fmt(s.drift_q)}`],
        ["Mean shift", s.mean_shift_pct != null ? `${fmt(s.mean_shift_pct)}%` : "–"], ["Drift ratio", s.drift_ratio], ["Variance ratio", s.variance_ratio],
        ["Path dispersion Δ", s.path_dispersion_gap]]} />
      {P.length > 0 && (
        <LineChart height={170} series={[{ label: "P (physical)", points: P.map((y, i) => ({ x: i, y })) }, { label: "Q (risk-neutral)", points: Q.map((y, i) => ({ x: i, y })), cls: "s2" }]}
          xLabel="step" caption="One path under each measure, from the same shocks" />
      )}
      <Sub>Terminal distribution</Sub>
      <Multiples rows={d.distribution_rows || []} labelKey="measure" keys={["mean", "variance", "q05", "q50", "q95"]} />
    </>
  );
}

const Results = () => (
  <>
    <Slot mode={ID} slot="pde" title="PDE">{(d) => <Pde data={d} />}</Slot>
    <Slot mode={ID} slot="convergence" title="Lattice convergence">{(d) => <Convergence data={d} />}</Slot>
    <Slot mode={ID} slot="benchmark" title="Method benchmark">{(d) => <Benchmark data={d} />}</Slot>
    <Slot mode={ID} slot="measure" title="Change of measure">{(d) => <Measure data={d} />}</Slot>
  </>
);

const about = "Check the numerical methods against each other: a finite-difference PDE solver, how the lattices converge as steps grow, every method against a baseline, and the change of measure from the physical to the risk-neutral world.";

export default { id: ID, label: "Numerics", about, Params, Results, run, precheck: true };
