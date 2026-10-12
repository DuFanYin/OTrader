// The run history in this browser: one entry per Run of a mode. An entry keeps only the run ids of its results (the
// results themselves stay on the server, GET /api/runs/{id}), so it is small; entries are kept in localStorage.
import { signal } from "@preact/signals";
import { get } from "./api.js";
import { results, runs } from "./store.js";

const KEY = "ql_history_v1";
const MAX_ENTRIES = 100;
const MAX_CHARS = 400_000;  // the whole history in localStorage, at most

function read() {
  try {
    const v = JSON.parse(localStorage.getItem(KEY) || "[]");
    return Array.isArray(v) ? v : [];
  } catch {
    return [];
  }
}

export const history = signal(read());
export const restored = signal(null);  // { mode, at } while the results shown came from the history

function save(list) {
  let kept = list.slice(0, MAX_ENTRIES);
  while (kept.length > 1 && JSON.stringify(kept).length > MAX_CHARS) kept = kept.slice(0, -1);
  history.value = kept;
  try { localStorage.setItem(KEY, JSON.stringify(kept)); } catch { /* full or blocked: the history lasts this visit */ }
}

const isEnvelope = (v) => !!v && typeof v === "object" && typeof v.run_id === "string" && "result_summary" in v;

/** Every result envelope in a slot's data, in order. */
export function envelopes(v) {
  if (isEnvelope(v)) return [v];
  if (Array.isArray(v)) return v.flatMap(envelopes);
  if (v && typeof v === "object") return Object.values(v).flatMap(envelopes);
  return [];
}

/** A slot's data with each envelope replaced by a reference to its run. */
function shrink(v) {
  if (isEnvelope(v)) return { $run: v.run_id };
  if (Array.isArray(v)) return v.map(shrink);
  if (v && typeof v === "object") return Object.fromEntries(Object.entries(v).map(([k, x]) => [k, shrink(x)]));
  return v;
}

/** The envelope of a run, from the server (get_run answers with the run inside its own envelope). */
export async function fetchRun(id) {
  const g = await get(`api/runs/${encodeURIComponent(id)}`);
  const s = g.result_summary;
  return { run_id: s.run_id, tool: s.tool, created_at: s.created_at, input_params: s.input_params,
    result_summary: s.result_summary, result_details: g.result_details, warnings: s.warnings, diagnostics: {} };
}

async function expand(v) {
  if (v && typeof v === "object" && typeof v.$run === "string") return fetchRun(v.$run);
  if (Array.isArray(v)) return Promise.all(v.map(expand));
  if (v && typeof v === "object") return Object.fromEntries(await Promise.all(Object.entries(v).map(async ([k, x]) => [k, await expand(x)])));
  return v;
}

const INPUT_KEYS = ["currency", "option_type", "spot", "strike", "maturity", "vol", "stress_pack"];
const HEADLINE_KEYS = ["bs", "net_bs_price", "price", "worst_scenario", "gate_decision", "n_returned", "winner_method", "pnl_std"];

function describe(env) {
  const p = env?.input_params || {}, s = env?.result_summary || {};
  const inputs = INPUT_KEYS.filter((k) => p[k] != null).map((k) => [k, p[k]]);
  if (Array.isArray(p.legs) && p.legs.length) inputs.push(["legs", p.legs.length]);
  const key = HEADLINE_KEYS.find((k) => s[k] != null);
  return { inputs: inputs.slice(0, 5), headline: key ? [key, s[key]] : null };
}

/** Adds the run of `mode` that just ended (its results, and its pre-check) to the history. */
export function record(mode) {
  const shrinkAll = (slots) => Object.fromEntries(Object.entries(slots || {})
    .filter(([, s]) => s.state === "ok").map(([name, s]) => [name, shrink(s.data)]));
  const slots = shrinkAll(results.value[mode]), pre = shrinkAll(results.value[`pre:${mode}`]);
  const first = Object.values(results.value[mode] || {}).flatMap((s) => (s.state === "ok" ? envelopes(s.data) : []))[0];
  if (!first) return;  // nothing the server keeps
  if (restored.value?.mode === mode) restored.value = null;
  save([{ id: first.run_id, mode, at: Date.now(), ...describe(first), slots, pre }, ...history.value]);
}

/** Shows an entry's results again: each card's run is fetched from the server. */
export async function load(entry) {
  const show = async (key, slots) => {
    const names = Object.keys(slots || {});
    results.value = { ...results.value, [key]: Object.fromEntries(names.map((n) => [n, { state: "running" }])) };
    await Promise.all(names.map(async (n) => {
      let value;
      try {
        value = { state: "ok", data: await expand(slots[n]) };
      } catch (err) {
        value = { state: "error", error: `This result is no longer kept on the server (${err?.message ?? err}). Run again.` };
      }
      results.value = { ...results.value, [key]: { ...results.value[key], [n]: value } };
    }));
  };
  runs.value = { ...runs.value, [entry.mode]: { running: false } };
  restored.value = { mode: entry.mode, at: entry.at };
  await Promise.all([show(entry.mode, entry.slots), show(`pre:${entry.mode}`, entry.pre)]);
}

export function forget(id) {
  save(history.value.filter((e) => e.id !== id));
}

export function clearHistory() {
  save([]);
}

/** The scalar leaves of a result summary, by dotted key (one level of nesting). */
function leaves(s) {
  const out = {};
  for (const [k, v] of Object.entries(s || {})) {
    if (v && typeof v === "object" && !Array.isArray(v)) {
      for (const [k2, v2] of Object.entries(v)) if (typeof v2 === "number" || typeof v2 === "string") out[`${k}.${k2}`] = v2;
    } else if (typeof v === "number" || typeof v === "string" || typeof v === "boolean") out[k] = v;
  }
  return out;
}

/** The rows of what differs between two runs of one card: inputs, then results. */
function differ(card, runA, runB) {
  const rows = [];
  for (const [part, a, b] of [["input", leaves(runA.input_params), leaves(runB.input_params)],
    ["result", leaves(runA.result_summary), leaves(runB.result_summary)]]) {
    for (const key of new Set([...Object.keys(a), ...Object.keys(b)])) {
      if (a[key] === b[key]) continue;
      const num = typeof a[key] === "number" && typeof b[key] === "number";
      rows.push({ slot: card, part, key, a: a[key], b: b[key], change: num ? b[key] - a[key] : null,
        rel: num && a[key] ? (b[key] - a[key]) / Math.abs(a[key]) : null });
    }
  }
  return rows;
}

/** What differs between two entries: per card, every input and headline number that changed. */
export async function compare(a, b) {
  const [ea, eb] = await Promise.all([expand(a.slots), expand(b.slots)]);
  return Object.keys(ea).filter((slot) => slot in eb).flatMap((slot) => {
    const [runsA, runsB] = [envelopes(ea[slot]), envelopes(eb[slot])];
    return runsA.flatMap((run, i) => (runsB[i] ? differ(runsA.length > 1 ? `${slot} ${i + 1}` : slot, run, runsB[i]) : []));
  });
}
