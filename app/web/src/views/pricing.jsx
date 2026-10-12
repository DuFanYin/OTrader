// Pricing: single (every method, Greeks, calibration, IV diagnostics, scenarios, Greek surfaces) or batch.
import { useState } from "preact/hooks";
import { post } from "../lib/api.js";
import { fixed, fmt, ms, pct, title } from "../lib/format.js";
import * as P from "../lib/payloads.js";
import { begin, fill } from "../lib/runner.js";
import { params } from "../lib/store.js";
import { Badge, Kv, Note, Slot, Sub, Table, tone } from "../ui/blocks.jsx";
import { Group, Num, Seg, Slider, Segmented } from "../ui/controls.jsx";
import { DialogGroup } from "../ui/dialog.jsx";
import { Columns, HBars } from "../charts/Bars.jsx";
import { Heatmap } from "../charts/Heatmap.jsx";
import { LineChart } from "../charts/LineChart.jsx";
import { MarketGroup, OptionGroup, PrecheckGroup } from "./shared.jsx";

const ID = "pricing";
const GREEKS = ["delta", "gamma", "theta", "vega", "rho"];

function Params() {
  const p = params.value, batch = p.pricingMode === "batch";
  return (
    <>
      <Group title="Run">
        <Seg k="pricingMode" label="What" options={[["single", "Single option"], ["batch", "Batch grid"]]} />
        {batch && <Num k="batchJobs" label="Jobs" step="1" />}
        {batch && <Num k="batchSpotShock" label="Spot shock" step="0.01" />}
      </Group>
      <MarketGroup />
      <OptionGroup />
      {!batch && (
        <DialogGroup title="IV diagnostics" summary={`smile ${p.ivSmilePoints} points · K ${p.ivKSteps} × T ${p.ivTSteps} · `
          + `Heston ${p.hestonMaxIter} iterations · burst ${p.burstLower}–${p.burstUpper}%`}>
          <Num k="ivSmilePoints" label="Smile points" step="2" min="5" max="21" />
          <Num k="ivKSteps" label="K steps" step="2" min="5" max="21" />
          <Num k="ivTSteps" label="T steps" step="2" min="5" max="21" />
          <Num k="hestonMaxIter" label="Heston max iter" step="50" min="50" max="5000" />
          <Slider k="burstLower" label="Burst lower quantile" min="0" max="49" unit="%" />
          <Slider k="burstUpper" label="Burst upper quantile" min="51" max="100" unit="%" />
        </DialogGroup>
      )}
      <PrecheckGroup />
    </>
  );
}

async function run() {
  const p = params.value;
  const b = P.base(p);
  if (p.pricingMode === "batch") {
    begin(ID, ["batch"]);
    await fill(ID, "batch", post("api/lab/pricing/batch/grid", { base: b, n_jobs: p.batchJobs, spot_shock: p.batchSpotShock }));
    return;
  }
  begin(ID, ["prices", "calibration", "iv", "scenario", "greeks"]);
  await Promise.all([
    fill(ID, "prices", post("api/lab/pricing/run", b)),
    fill(ID, "calibration", Promise.allSettled([post("api/lab/calibration/iv", P.bsImpliedVol(p)), post("api/lab/calibration/heston", P.heston(p))])
      .then(([iv, heston]) => {
        if (iv.status === "rejected" && heston.status === "rejected") throw iv.reason;
        return { iv: iv.value, heston: heston.value, error: [iv, heston].find((x) => x.status === "rejected")?.reason?.message };
      })),
    fill(ID, "iv", post("api/lab/market/iv/diagnostics", P.ivDiagnostics(p))),
    fill(ID, "scenario", post("api/lab/scenario/run", b)),
    fill(ID, "greeks", Promise.all(GREEKS.map((g) => post("api/lab/greek/surface", P.greekSurface(p, g))))),
  ]);
}

// ── results

function Prices({ data }) {
  const s = data.result_summary || {}, g = s.greeks || {}, e = s.error_decomposition || {};
  return (
    <>
      <div class="flex flex-wrap items-baseline gap-x-4 gap-y-1">
        <span class="text-2xl font-semibold tracking-tight">{fmt(s.bs)}</span>
        <span class="text-sm text-muted">Black-Scholes · {(s.option_type || "call").toUpperCase()}</span>
      </div>
      <Kv rows={[
        ["Monte Carlo", s.mc], ["Binomial", s.binomial], ["Trinomial", s.trinomial],
        ["Method spread", tone(s.method_spread, Math.abs(s.method_spread) < 0.01)],
        ["Relative spread", tone(s.relative_spread, Math.abs(s.relative_spread) < 0.005)],
        ["MC std error", s.mc_std_err],
      ]} />
      <Sub>Errors against Black-Scholes</Sub>
      <Kv rows={[["MC 95% CI", `${fmt(s.mc_ci_low)} – ${fmt(s.mc_ci_high)}`], ["MC − BS", e.mc_minus_bs], ["Binomial − BS", e.binomial_minus_bs],
        ["Trinomial − BS", s.trinomial_minus_bs]]} />
      <Sub>Greeks (BS)</Sub>
      <Kv rows={[["Δ delta", g.delta_bs], ["Γ gamma", g.gamma_bs], ["Θ theta", g.theta_bs], ["ν vega", g.vega_bs], ["ρ rho", g.rho_bs]]} />
      {s.american_methods ? <American a={s.american_methods} /> : s.american != null && <Kv rows={[["American (binomial)", s.american]]} />}
    </>
  );
}

function American({ a }) {
  const ref = Number(a.binomial);
  const rows = [
    { m: "Binomial (CRR)", v: a.binomial, note: "" },
    { m: "Trinomial (Boyle)", v: a.trinomial, note: "" },
    { m: "PDE CN + PSOR", v: a.pde_psor, note: `${a.pde_psor_iterations ?? "–"} sweeps` },
    { m: "Longstaff-Schwartz", v: a.lsm, note: `±${fmt(1.96 * Number(a.lsm_std_err))} · ${a.lsm_paths ?? "–"} paths` },
  ];
  return (
    <>
      <Sub>American exercise · premium {fmt(a.early_exercise_premium)}</Sub>
      <Table compact rows={rows} cols={[
        { key: "m", label: "Method", align: "left" }, { key: "v", label: "Price" },
        { key: "v", label: "vs binomial", fmt: (v) => (ref ? pct((Number(v) - ref) / ref, 3) : "–") }, { key: "note", label: "", cls: "text-muted" },
      ]} />
    </>
  );
}

function Calibration({ data }) {
  const iv = data.iv?.result_summary, h = data.heston?.result_summary, hd = data.heston?.result_details || {};
  const quality = h?.fit_quality;
  return (
    <>
      {iv && <Kv rows={[["BS implied vol", iv.implied_vol ?? iv.ivs?.[0]], ["Converged", iv.converged ?? iv.n_converged > 0], ["Error", iv.final_error ?? 0]]} />}
      {h && (
        <>
          <div class="flex items-center gap-2">
            <Sub>Heston</Sub>
            <Badge kind={quality === "good" ? "good" : quality === "fair" ? "warn" : "bad"}>{quality ? `${quality} fit` : "fit unknown"}</Badge>
            <span class="text-xs text-muted">RMSE {fmt(h.rmse)}{h.rmse_rel != null && ` (${pct(h.rmse_rel, 2)} of the average quote)`} · max {fmt(h.max_abs_error)} · {h.iterations} iterations</span>
          </div>
          <Kv rows={[["v₀", h.v0], ["κ mean reversion", h.kappa], ["θ long variance", h.theta], ["ξ vol of vol", h.xi], ["ρ correlation", h.rho]]} />
          {hd.residuals?.length > 0 && <Columns values={hd.residuals} caption="Residual per quote (model − market): 7 strikes × 2 maturities" />}
        </>
      )}
      {data.error && <Note>{data.error}</Note>}
    </>
  );
}

function Scenario({ data }) {
  const s = data.result_summary || {};
  const pts = (data.result_details?.tornado_points || []).map((r) => ({ label: r.label ?? r.name, value: r.value }));
  return (
    <>
      <Kv rows={[["Base price", s.base_price], ["Range", s.price_range], ["Low", s.min_price], ["High", s.max_price]]} />
      <HBars signed rows={pts} caption="Black-Scholes price change against the base, per scenario" />
    </>
  );
}

function Iv({ data }) {
  const s = data.result_summary || {}, d = data.result_details || {};
  const smiles = d.smiles_by_expiry || {};
  const expiries = Object.keys(smiles);
  const [expiry, setExpiry] = useState(s.smile_selected_expiry || expiries[0] || "");
  const smile = (expiries.length ? smiles[expiry] : d.smile_rows) || [];
  const term = [...(d.term_structure_rows || [])].sort((a, b) => a.years - b.years);
  const grid = d.surface_grid_rows || [];
  const ks = [...new Set(grid.map((r) => Number(r.k_ratio)))].sort((a, b) => a - b);
  const ts = [...new Set(grid.map((r) => Number(r.years)))].sort((a, b) => a - b);
  const cell = new Map(grid.map((r) => [`${Number(r.k_ratio)}|${Number(r.years)}`, r]));
  const highlighted = term.find((r) => r.expiry === expiry)?.years;
  return (
    <>
      <Kv rows={[
        ["Interpolated IV", s.interp_iv_pct != null ? `${Number(s.interp_iv_pct).toFixed(2)}%` : "–"], ["Method", s.interp_method],
        ["Skew slope", s.local_skew_slope], ["Curvature", s.local_curvature],
        ["Confidence", s.confidence_label ? `${s.confidence_label} (${fmt(s.confidence_score)})` : "–"], ["Smile expiry", s.smile_selected_expiry],
      ]} />
      <div class="grid gap-4 lg:grid-cols-2">
        <div class="min-w-0">
          <Sub>Term structure · ATM IV</Sub>
          <LineChart height={190} series={[{ label: "ATM IV", points: term.map((r) => ({ x: r.years, y: r.iv * 100, label: r.expiry })), dots: true, area: true }]}
            xLabel="maturity (years)" yFmt={(v) => `${(+v).toFixed(0)}%`} valueFmt={(v) => `${(+v).toFixed(2)}%`} highlight={highlighted} />
        </div>
        <div class="min-w-0">
          <div class="flex items-center justify-between gap-2">
            <Sub>Smile</Sub>
            {expiries.length > 1 && (
              <select class="h-6 rounded-sm border border-line bg-surface px-1.5 text-xs" value={expiry} onChange={(e) => setExpiry(e.currentTarget.value)}>
                {expiries.map((k) => <option value={k}>{k}</option>)}
              </select>
            )}
          </div>
          <LineChart height={190} series={[{ label: "IV", points: [...smile].sort((a, b) => a.log_moneyness - b.log_moneyness)
            .map((r) => ({ x: r.log_moneyness, y: r.iv * 100, label: r.strike ? `K ${Math.round(r.strike).toLocaleString()}` : undefined })), dots: true, area: true }]}
            xLabel="log-moneyness ln(K/S)" marks={[{ x: 0, label: "ATM" }]} yFmt={(v) => `${(+v).toFixed(0)}%`} valueFmt={(v) => `${(+v).toFixed(2)}%`} />
        </div>
      </div>
      {grid.length > 0 && (
        <>
          <Sub>Surface · K/S × T</Sub>
          <Heatmap xs={ks} ys={ts} value={(i, j) => cell.get(`${ks[i]}|${ts[j]}`)?.iv}
            outline={(i, j) => { const z = cell.get(`${ks[i]}|${ts[j]}`)?.zone; return z === "burst_high" ? "var(--down)" : z === "suppressed_low" ? "var(--accent)" : null; }}
            xFmt={(v) => (+v).toFixed(2)} yFmt={(v) => `${(+v).toFixed(2)}y`} cellFmt={(v) => `${(v * 100).toFixed(1)}`} legendFmt={(v) => pct(v)}
            xLabel="K / S" caption="Colour: IV. Red border: burst (above the upper quantile); blue border: suppressed (below the lower one)." />
        </>
      )}
    </>
  );
}

function Greeks({ data }) {
  const [greek, setGreek] = useState("delta");
  const surface = data.find((d) => d?.result_summary?.greek === greek) || data[0];
  const s = surface?.result_summary || {}, d = surface?.result_details || {};
  const nt = d.maturities?.length || 0;
  return (
    <>
      <Segmented size="xs" options={GREEKS.map((g) => [g, title(g)])} value={greek} onChange={setGreek} />
      <Heatmap xs={d.spots || []} ys={d.maturities || []} value={(i, j) => d.grid?.[i * nt + j]}
        palette={greek === "delta" || greek === "rho" || greek === "theta" ? "diverging" : "sequential"}
        center={greek === "delta" ? (s.grid_min < 0 ? 0 : 0.5) : greek === "theta" || greek === "rho" ? 0 : undefined}
        xFmt={(v) => Math.round(v).toLocaleString()} yFmt={(v) => `${(+v).toFixed(2)}y`} cellFmt={(v) => fixed(v, 3)}
        xLabel="spot" yLabel="maturity" caption={`${title(greek)} over spot (60–140% of spot) × maturity · range ${fmt(s.grid_min)} to ${fmt(s.grid_max)}`} />
    </>
  );
}

function Batch({ data }) {
  const s = data.result_summary || {}, d = data.result_details || {};
  const rows = d.flat_rows || [];
  return (
    <>
      <Kv rows={[["Jobs", s.job_count], ["Total compute", ms(s.total_compute_ms)], ["Mean spread", s.avg_method_spread], ["Median spread", s.p50_method_spread],
        ["95th pct spread", s.p95_method_spread], ["Max spread", s.max_method_spread], ["Spread std dev", s.spread_std], ["Spread CV", s.spread_cv],
        ["Worst job", s.worst_spread_job_index], ["Best job", s.best_spread_job_index]]} />
      <Sub>Method summary</Sub>
      <Table compact rows={d.method_summary || []} cols={[{ key: "method", label: "Method", align: "left" }, { key: "avg", label: "Mean" }, { key: "min", label: "Min" },
        { key: "max", label: "Max" }, { key: "std", label: "Std dev" }, { key: "avg_err_vs_bs", label: "Mean err vs BS" }]} />
      <LineChart height={160} series={[{ label: "spread", points: rows.map((r, i) => ({ x: i, y: Number(r.spread) })), dots: true }]}
        xLabel="job" caption="Method spread per job of the spot × vol grid" />
    </>
  );
}

function Results() {
  const batch = params.value.pricingMode === "batch";
  if (batch) return <Slot mode={ID} slot="batch" title="Batch pricing" wide>{(d) => <Batch data={d} />}</Slot>;
  return (
    <>
      <Slot mode={ID} slot="prices" title="Price">{(d) => <Prices data={d} />}</Slot>
      <Slot mode={ID} slot="calibration" title="Calibration">{(d) => <Calibration data={d} />}</Slot>
      <Slot mode={ID} slot="scenario" title="Scenario sweep">{(d) => <Scenario data={d} />}</Slot>
      <Slot mode={ID} slot="greeks" title="Greek surfaces" meta={null}>{(d) => <Greeks data={d} />}</Slot>
      <Slot mode={ID} slot="iv" title="Implied volatility" wide>{(d) => <Iv data={d} />}</Slot>
    </>
  );
}

const about = "Price an option every way the engine knows: Black-Scholes, Monte Carlo, binomial and trinomial trees, with all five Greeks. Calibrate Black-Scholes and Heston to the market, sweep scenarios and map the implied-volatility surface. In Live mode spot, volatility and rates come from Deribit and the US Treasury curve.";

export default { id: ID, label: "Pricing", about, Params, Results, run, precheck: true };
