// Experiments: a strategy over a space of its settings, every trial registered (and counted in its family) before it
// runs, the trials run in the background by workers; then what the results say once the tries are counted.
import { signal } from "@preact/signals";
import { get, post } from "../lib/api.js";
import { catalog, loadCatalog, parametersOf } from "../lib/catalog.js";
import { files, loadFiles } from "../lib/data.js";
import { count, dayOf, fixed, usd } from "../lib/format.js";
import { dropSample, shown } from "../lib/samples.js";
import { params, setParam } from "../lib/store.js";
import { Badge, Card, ErrorText, Kv, Note, Running, Signed, Table, Warnings } from "../ui/blocks.jsx";
import { Day, Group, Line, Num, Seg, Select, SmallButton, Text } from "../ui/controls.jsx";
import { DialogGroup } from "../ui/dialog.jsx";
import { Costs, changedCosts } from "./parameters.jsx";

// What the results pane shows: the experiments, the one chosen with its trials, days and analysis.
const view = signal({ list: null, chosen: null, experiment: null, analysis: null, matrix: null, error: null });
/** Set by the page: opens a trial in Backtest (its inputs: symbol, days, strategy, setting, costs) and runs it. */
export const toBacktest = { open: null };
const busy = signal(false);
let polling = null;

const symbolOf = (name) => files.value.symbols.find((s) => s.name === name);

async function enter() {
  if (files.value.state === "idle") await loadFiles();
  if (catalog.value.state === "idle") await loadCatalog();
  const p = params.value;
  if (!p.exSymbol && files.value.symbols[0]) setParam("exSymbol", files.value.symbols[0].name);
  if (!p.exStrategy && catalog.value.list.length) setParam("exStrategy", catalog.value.list[0].name);
  // The sample stays on screen until there is an experiment of one's own to show.
  const out = await get("api/experiments?limit=50");
  if (shown.value.experiments && !out.result_summary.experiments.length) return;
  dropSample("experiments");
  view.value = { ...view.value, list: out.result_summary, error: null };
}

/** A parameter's space from its text: "" its default, "15, 30, 60" those values, "10..60" sampled in the range,
 * "10..60/10" the range on a grid of that step. */
function spaceOf(text) {
  const t = String(text ?? "").trim();
  if (!t) return undefined;
  const range = t.match(/^(-?[\d.]+)\s*\.\.\s*(-?[\d.]+)(?:\s*\/\s*([\d.]+))?$/);
  if (range) {
    const [, lo, hi, step] = range;
    return step ? { range: [Number(lo), Number(hi)], step: Number(step) } : { range: [Number(lo), Number(hi)] };
  }
  const values = t.split(",").map((v) => Number(v.trim()));
  if (values.some((v) => !Number.isFinite(v))) throw new Error(`"${t}": numbers separated by commas, or lo..hi, or lo..hi/step`);
  return values.length === 1 ? values[0] : values;
}

const spaceTexts = () => params.value.exSpace?.[params.value.exStrategy] ?? {};

async function submit() {
  dropSample("experiments");
  const p = params.value;
  if (!p.exFamily?.trim()) throw new Error("Name the family: the hypothesis these trials are variants of.");
  if (!p.exStrategy || !p.exSymbol) throw new Error("Choose a symbol and a strategy first.");
  const space = {};
  for (const [name, text] of Object.entries(spaceTexts())) {
    const s = spaceOf(text);
    if (s !== undefined) space[name] = s;
  }
  busy.value = true;
  try {
    const out = await post("api/experiments", {
      family: p.exFamily.trim(), strategy: p.exStrategy, space, search: p.exSearch, n: p.exN ?? 0, seed: p.exSeed ?? 0,
      symbol: p.exSymbol, start: p.exStart || null, end: p.exEnd || null,
      costs: p.exCosts ?? {}, note: p.exNote ?? "",
    });
    await refresh();
    await choose(out.result_summary.experiment_id);
  } finally {
    busy.value = false;
  }
}

async function refresh() {
  try {
    const out = await get("api/experiments?limit=50");
    view.value = { ...view.value, list: out.result_summary, error: null };
  } catch (err) {
    view.value = { ...view.value, error: err.message };
  }
}

async function choose(id) {
  view.value = { ...view.value, chosen: id, experiment: null, analysis: null, matrix: null };
  await load(id);
}

async function load(id) {
  const out = await get(`api/experiments/${id}`);
  if (view.value.chosen !== id) return;
  view.value = { ...view.value, experiment: out };
  const pending = out.result_summary.trials - out.result_summary.done;
  clearTimeout(polling);
  const stillRunning = Object.entries(out.result_summary.tasks ?? {}).some(([s, n]) => n > 0 && (s === "queued" || s === "leased"));
  if (stillRunning) polling = setTimeout(() => load(id), 5000);
  if (out.result_summary.done > 1 && !pending) await analyse(id);
}

async function analyse(id) {
  try {
    const out = await get(`api/experiments/${id}/analysis`);
    if (view.value.chosen === id) view.value = { ...view.value, analysis: out.result_details };
  } catch (err) {
    if (view.value.chosen === id) view.value = { ...view.value, analysis: { why: err.message } };
  }
}

async function robustness(id) {
  const out = await post(`api/experiments/${id}/robustness`, {});
  await refresh();
  return out;
}

// ── inputs

function SpaceEditor() {
  const p = params.value, declared = parametersOf(p.exStrategy), texts = spaceTexts();
  const set = (name, text) => setParam("exSpace", { ...p.exSpace, [p.exStrategy]: { ...texts, [name]: text } });
  const varied = Object.entries(texts).filter(([, t]) => String(t ?? "").trim());
  const summary = !p.exStrategy ? "Choose a strategy" : varied.length
    ? varied.map(([k, t]) => `${k} ${t}`).join(" · ") : "every parameter at its default";
  return (
    <DialogGroup title="Space" summary={summary} width="620px">
      <Line>Per parameter: empty for its default, 15, 30, 60 for those values, 10..60 to sample the range (random or Latin
        search), 10..60/10 for a grid of that step.</Line>
      {varied.length > 0 && (
        <div class="col-span-full flex justify-end">
          <SmallButton onClick={() => setParam("exSpace", { ...p.exSpace, [p.exStrategy]: {} })}>All to defaults</SmallButton>
        </div>
      )}
      {declared.map((d) => (
        <Text label={d.name} value={texts[d.name] ?? ""} onChange={(t) => set(d.name, t)} wide={false}
          placeholder={String(d.default)} hint={`${d.name}: default ${d.default}, from ${d.min} to ${d.max}${d.integer ? ", whole" : ""}`} />
      ))}
    </DialogGroup>
  );
}

function Params() {
  const p = params.value, s = symbolOf(p.exSymbol);
  const symbols = files.value.symbols.map((x) => [x.name, x.name]);
  const strategies = catalog.value.list.map((x) => [x.name, x.name]);
  return (
    <>
      <Group title="Hypothesis">
        <Text k="exFamily" label="Family" placeholder="0DTE straddle timing"
          hint="The hypothesis the trials are variants of: its trials are counted together, against its budget" />
        <Text k="exNote" label="Note" placeholder="What this experiment is for" />
      </Group>
      <Group title="Data">
        <Select k="exSymbol" label="Symbol" options={symbols} />
        {s && <Line>{s.date_start} to {s.date_end} · {s.number_of_days} days; the holdout is left out.</Line>}
        <Day k="exStart" label="First day" min={s?.date_start} max={s?.date_end} />
        <Day k="exEnd" label="Last day" min={s?.date_start} max={s?.date_end} />
      </Group>
      <Group title="Strategy">
        <Select k="exStrategy" label="Class" options={strategies} />
      </Group>
      <SpaceEditor />
      <Group title="Search">
        <Seg k="exSearch" label="How" options={[["grid", "Grid"], ["random", "Random"], ["latin", "Latin"]]} />
        {p.exSearch !== "grid" && <Num k="exN" label="Settings" min={1} step="1" />}
        {p.exSearch !== "grid" && <Num k="exSeed" label="Seed" step="1" />}
      </Group>
      <Costs storeKey="exCosts" />
    </>
  );
}

// ── results

const status = (r) => (r.pending ? <Badge kind="warn">{count(r.pending)} pending</Badge>
  : r.failed ? <Badge kind="bad">{count(r.failed)} failed</Badge> : <Badge kind="good">done</Badge>);

function List() {
  const v = view.value, rows = v.list?.experiments ?? [];
  const chosen = rows.findIndex((r) => r.id === v.chosen);
  return (
    <Card title="Experiments" meta={`${rows.length} latest · click one to open it`} actions={<SmallButton onClick={refresh}>Refresh</SmallButton>}>
      <Table compact maxHeight="196px" rows={rows} selected={chosen} onRow={(r) => choose(r.id)} empty="No experiment yet" cols={[
        { key: "created_at", label: "Submitted", align: "left", fmt: (t) => t?.replace("T", " ").slice(5, 16) },
        { key: "family", label: "Family", align: "left" },
        { key: "trials", label: "Trials", fmt: (n, r) => (r.done < n ? `${count(r.done)} / ${count(n)}` : count(n)) },
        { key: "days", label: "Days", fmt: count },
        { key: "id", label: "", fmt: (_, r) => status(r) },
      ]} />
      <Families families={v.list?.families ?? {}} />
    </Card>
  );
}

function Families({ families }) {
  const rows = Object.entries(families).map(([name, f]) => ({ name, ...f }));
  if (!rows.length) return null;
  const text = `Trials used of each family's budget: ${rows.map((f) => `${f.name} ${f.used} of ${f.budget}`).join(" · ")}. `
    + "Every trial registered counts, run or not.";
  return (
    <p class="line-clamp-2 text-xs leading-relaxed text-muted" title={text}>{text}</p>
  );
}

/** A setting as text: only the parameters in `keys` (those the experiment varied) when given. */
const settingText = (s, keys) => Object.entries(s ?? {}).filter(([k]) => !keys?.length || keys.includes(k))
  .map(([k, v]) => `${k} ${v}`).join(", ") || "defaults";

/** The parameters whose values differ between the trials (a setting has every parameter of its class). */
const variedKeys = (trials) => Object.keys(trials[0]?.setting ?? {})
  .filter((k) => new Set(trials.map((t) => t.setting?.[k])).size > 1);


/** The trial in Backtest: the experiment's symbol, days and costs, the trial's setting. */
function openTrial(experiment, trial) {
  const s = experiment.result_summary, spec = experiment.result_details.spec, day = dayOf;
  toBacktest.open?.({
    btSymbol: (s.first_day ?? "").split("/")[0], btStart: day(s.first_day), btEnd: day(s.last_day), btStrategy: s.strategy,
    btSettings: { ...params.value.btSettings, [s.strategy]: experiment.result_details.trials.find((t) => t.id === trial.id)?.setting ?? {} },
    btCosts: changedCosts(spec.costs),
    btRiskFreeRate: spec.pricing?.risk_free_rate ?? 0.05, btIvMode: spec.pricing?.iv_price_mode ?? "mid",
  });
}

function Trials({ experiment }) {
  const s = experiment.result_summary, trials = experiment.result_details.trials;
  const keys = variedKeys(trials);
  const rows = trials.filter((t) => t.summary).map((t) => ({ ...t, ...t.summary, setting: settingText(t.setting, keys) }))
    .sort((a, b) => b.sharpe - a.sharpe);
  const progress = `${count(s.done)} of ${count(s.trials)} trials done`;
  return (
    <Card title="Trials" meta={`${progress} · click one to open it in Backtest`}
      actions={s.done > 0 && <SmallButton onClick={() => robustness(s.id)}
        title="Queue the checks that need new runs on the best trial: fees and fill positions doubled, fills of the size shown a minute late, every leg at its bid or ask (when it filled inside the spread), each varied parameter 20% down and up">Robustness checks</SmallButton>}>
      {s.done < s.trials && <Running />}
      <Table compact maxHeight="300px" rows={rows} empty="No trial has finished" onRow={(t) => openTrial(experiment, t)} cols={[
        { key: "setting", label: "Setting", align: "left" },
        { key: "net", label: "Net", fmt: (v) => <Signed value={v} format={usd} /> },
        { key: "sharpe", label: "Sharpe", fmt: (v) => fixed(v, 2) },
        { key: "drawdown", label: "Drawdown", fmt: usd },
        { key: "worst_day", label: "Worst day", fmt: (v) => (v == null ? "–" : usd(v)) },
        { key: "es95_day", label: "ES95 day", fmt: (v) => (v == null ? "–" : usd(v)) },
        { key: "trades", label: "Trades", fmt: count },
        { key: "t", label: "t", fmt: (v) => fixed(v, 2) },
      ]} />
      <Warnings items={trials.filter((t) => t.error).slice(0, 3).map((t) => `${settingText(t.setting, keys)}: ${t.error}`)} />
      {s.note && <Note>{s.note}</Note>}
    </Card>
  );
}

/** The experiment's verdict once its tries are counted, beside the list: what was run, the best trial and whether it
 * passes. */
function Verdict({ a, experiment }) {
  const s = experiment.result_summary, trials = experiment.result_details.trials, keys = variedKeys(trials);
  const title = `${s.strategy} · ${s.family}`, meta = `${s.days} days, ${dayOf(s.first_day)} to ${dayOf(s.last_day)}`;
  if (!a) return <Card title={title} meta={meta}><Running /></Card>;
  if (a.why) return <Card title={title} meta={meta}><Note>{a.why}</Note></Card>;
  const d = a.deflated_sharpe, o = a.pbo, w = a.walk_forward;
  const verdict = (ok) => (ok == null ? "–" : ok ? <Badge kind="good">passes</Badge> : <Badge kind="bad">fails</Badge>);
  return (
    <Card title={title} meta={meta}>
      <Kv cols={2} rows={[
        ["Best trial", settingText(a.best.setting, keys)],
        ["Its Sharpe (a year)", fixed(d.sharpe * Math.sqrt(252), 2)],
        ["Best of the tries by luck", fixed(d.expected_max_sharpe * Math.sqrt(252), 2)],
        ["Deflated Sharpe", <>{fixed(d.dsr, 3)} {verdict(d.dsr == null ? null : d.dsr > 0.95)}</>],
        ["Overfitting probability", <>{fixed(o.pbo, 2)} {verdict(o.pbo == null ? null : o.pbo < 0.2)}</>],
        ["Walk-forward net", <Signed value={w.net} format={usd} />],
      ]} />
      <Note>{count(a.tries)} tries in the ledger, {count(a.family_tries)} in this family. Deflated Sharpe: the chance the best
        trial beats the best of all those tries with no skill (the bar is 0.95). Overfitting probability: how often the best
        in sample lands in the bottom half out of sample, over {count(o.splits)} splits into {o.blocks} blocks (noise gives
        0.5; the bar is 0.2).</Note>
    </Card>
  );
}

/** The walk-forward and the best trial taken apart, one column. */
function Checks({ a, experiment }) {
  if (!a || a.why) return null;
  const trials = experiment?.result_details.trials ?? [], keys = variedKeys(trials);
  const trialSetting = (id) => trials.find((t) => t.id === id)?.setting;
  const w = a.walk_forward, r = a.by_regime ?? {}, b = a.without_best_days;
  return (
    <div class="flex min-w-0 flex-col gap-3">
      <Card title="Walk-forward" meta={`train ${w.train} days, trade the next ${w.test} · ${w.positive_windows} of ${w.windows.length} windows positive`}>
        <Table compact maxHeight="150px" rows={w.windows} cols={[
          { key: "test_days", label: "Traded", align: "left", fmt: (t) => t?.join(" to ") },
          { key: "trial_id", label: "Trial picked", align: "left", fmt: (id) => settingText(trialSetting(id), keys) },
          { key: "test_net", label: "Net", fmt: (v) => <Signed value={v} format={usd} /> },
        ]} />
      </Card>
      <Card title="The best trial, taken apart" meta="up, down: the day's move; quiet, wild: its smaller, larger half">
        <Kv cols={2} rows={[
          [`Without its ${b.days_dropped} best days`, <Signed value={b.net} format={usd} />],
          ["With them", <Signed value={b.net_with_them} format={usd} />],
          ...["up", "down", "quiet", "wild"].filter((k) => r[k]).map((k) => [`${k[0].toUpperCase()}${k.slice(1)} days (${r[k].days})`,
            <Signed value={r[k].net} format={usd} />]),
        ]} />
      </Card>
    </div>
  );
}

function Results() {
  const v = view.value;
  return (
    <>
      {v.error && <Card title="Experiments" wide><ErrorText>{v.error}</ErrorText></Card>}
      {/* the list and the verdict side by side, then the trials beside the checks: the answer on the first screen */}
      <List />
      {v.chosen && !v.experiment && <Card title="Experiment"><Running /></Card>}
      {v.experiment && <Verdict a={v.analysis} experiment={v.experiment} />}
      {v.experiment && <Trials experiment={v.experiment} />}
      {v.experiment && <Checks a={v.analysis} experiment={v.experiment} />}
    </>
  );
}

export const experiments = {
  id: "experiments",
  label: "Experiments",
  about: "A strategy over a space of its settings, each trial registered and counted in its family before it runs, run in "
    + "the background by workers (each day loaded and priced once for all the trials); then the deflated Sharpe ratio, "
    + "the probability of backtest overfitting and a walk-forward, with the family's tries counted.",
  enter,
  Params,
  Results,
  // An experiment to show: one of one's own, or the sample's.
  hasResults: () => view.value.chosen != null || (view.value.list?.experiments?.length ?? 0) > 0,
  status: () => "",
  action: { label: "Submit experiment", busyLabel: "Submitting…", busy, run: submit },
  sample: { dump: () => view.value, show: (saved) => { view.value = saved; } },
};
