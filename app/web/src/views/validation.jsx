// Validation: the gate on its own (statistics, Itô, simulation, lattice checks against thresholds), and
// the same report as a pre-check before the other modes.
import { post } from "../lib/api.js";
import { fixed, fmt } from "../lib/format.js";
import * as P from "../lib/payloads.js";
import { begin, fill, put } from "../lib/runner.js";
import { params } from "../lib/store.js";
import { Badge, Note, Slot, Sub, Table } from "../ui/blocks.jsx";
import { Chips, Group, Num, Seg } from "../ui/controls.jsx";
import { DialogGroup } from "../ui/dialog.jsx";
import { CHECKS, MarketGroup } from "./shared.jsx";

const ID = "validation";

const ITO_FUNCTIONS = [["exp_martingale", "exp martingale"], ["w2_minus_t", "W² − t"], ["w3", "W³"]];

function Params() {
  const p = params.value;
  return (
    <>
      <Group title="Checks"><Chips items={CHECKS} /></Group>
      <DialogGroup title="Statistics" summary={`θ ${p.statsTheta} · sample ${Number(p.statsN).toLocaleString()}`}>
        <Num k="statsTheta" label="θ" step="0.1" />
        <Num k="statsN" label="Sample" step="1000" />
      </DialogGroup>
      <DialogGroup title="Itô" summary={`${ITO_FUNCTIONS.find(([v]) => v === p.itoFunction)?.[1] ?? p.itoFunction} · θ ${p.itoTheta} · t ${p.itoT} · ${p.itoN} steps`}>
        <Num k="itoTheta" label="θ" step="0.1" />
        <Num k="itoT" label="t" step="0.1" />
        <Num k="itoN" label="Steps" step="500" wide />
        <Seg k="itoFunction" label="Function" options={ITO_FUNCTIONS} />
      </DialogGroup>
      <DialogGroup title="Simulation" summary={`GBM · ${p.simSteps} steps · hedged ${p.nReb} times on ${p.hedgePaths} paths`}>
        <Num k="simSteps" label="Steps" step="50" />
        <Num k="nReb" label="Rebalances" step="1" />
        <Num k="hedgePaths" label="Paths" step="100" wide />
      </DialogGroup>
      <MarketGroup drift />
    </>
  );
}

/** Runs the gate into `slot` of `mode`; returns whether it passed (true when nothing is picked). */
export async function gate(mode, slot) {
  const p = params.value;
  if (!P.anyCheck(p)) {
    put(mode, slot, { empty: true });
    return true;
  }
  const d = await fill(mode, slot, post("api/lab/validation/gate", P.validationGate(p)));
  return d?.result_summary?.gate_decision === "go";
}

async function run() {
  begin(ID, ["gate"]);
  await gate(ID, "gate");
}

const show = (v) => (v == null ? "–" : typeof v === "object" ? Object.entries(v).map(([k, x]) => (Array.isArray(v) ? x : `${k} ${x}`)).join(Array.isArray(v) ? "," : ", ") : fmt(v));
const decision = (g) => <Badge kind={g === "go" || g === "pass" ? "good" : g === "fail" || g === "blocked" ? "bad" : "warn"}>{g ?? "–"}</Badge>;

/** A check's value as a share of its threshold: a bar to twice the threshold, with a tick at the threshold. */
function Used({ row }) {
  const used = Math.abs(Number(row.value)) / Math.max(Math.abs(Number(row.threshold)), 1e-12), pass = row.status === "pass";
  return (
    <span class="inline-flex items-center gap-2">
      <span class="relative h-1.5 w-28 rounded-full bg-surface-3">
        <span class={`absolute inset-y-0 left-0 rounded-full ${pass ? "bg-up" : "bg-down"}`} style={{ width: `${Math.max(2, Math.min(used, 2) * 50)}%` }} />
        <span class="absolute -inset-y-1 left-1/2 w-px bg-line-strong" />
      </span>
      <span class={`w-10 text-right text-xs tabular-nums ${pass ? "text-muted" : "text-down"}`}>{used > 2 ? `${fixed(used, 1)}×` : `${Math.round(used * 100)}%`}</span>
    </span>
  );
}

/** The full report; `compact` (a pre-check) folds the details. */
export function Report({ data, compact }) {
  if (data.empty) return <Note>No check picked: Stats, Itô, Simulation or Lattice.</Note>;
  const s = data.result_summary || {}, d = data.result_details || {};
  const rows = d.rows || [];
  const failing = rows.filter((r) => r.status === "fail"), failed = s.checks_failed ?? failing.length;
  const head = (
    <div class="flex flex-wrap items-center gap-2 text-sm">
      {decision(s.gate_decision)}
      <span>{(s.checks_total ?? rows.length) - failed}/{s.checks_total ?? rows.length} checks passed</span>
      {failing.length > 0 && <span class="text-muted">· failed: {failing.map((r) => `${r.capability} ${r.metric}`).join(", ")}</span>}
    </div>
  );
  const details = (
    <>
      <Table compact rows={rows} cols={[
        { key: "metric", label: "Check", align: "left", fmt: (v, r) => <span><span class="font-medium">{v}</span> <span class="text-muted">· {r.capability}</span></span> },
        { key: "interpretation", label: "What it tests", align: "left", cls: "text-muted whitespace-normal" },
        { key: "value", label: "Value" },
        { key: "threshold", label: "Threshold" },
        { key: "excess", label: "Of the threshold", fmt: (v, r) => <Used row={r} /> },
      ]} />
      {failing.length > 0 && (
        <>
          <Sub>What to change</Sub>
          <ol class="m-0 flex list-decimal flex-col gap-1 pl-5 text-sm">
            {failing.map((r) => (
              <li><span class="font-medium">{r.metric}</span>: {r.action}{r.param && r.proposed_value != null ? <span class="text-muted"> · {r.param} {show(r.current_value)} → {show(r.proposed_value)}</span> : null}</li>
            ))}
          </ol>
        </>
      )}
    </>
  );
  if (!compact) return <>{head}{details}</>;
  return (
    <details class="group">
      <summary class="flex cursor-pointer list-none items-center justify-between gap-3">{head}
        <span class="shrink-0 text-xs text-accent"><span class="group-open:hidden">Show details</span><span class="hidden group-open:inline">Hide details</span></span>
      </summary>
      <div class="mt-3 flex flex-col gap-3">{details}</div>
    </details>
  );
}

const Results = () => <Slot mode={ID} slot="gate" title="Validation gate" wide>{(d) => <Report data={d} />}</Slot>;

const about = "Run the theory checks on their own: sample moments against the normal distribution, Itô's formula on simulated paths, simulation and hedging sanity, and the lattices against closed forms. Each is held to a threshold, with ranked fixes for whatever fails.";

export default { id: ID, label: "Validation", about, Params, Results, run, precheck: false };
