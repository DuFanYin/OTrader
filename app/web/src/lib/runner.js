// Run bookkeeping: each mode's results are named slots (one per request), each running, ok or failed.
import { results, runs } from "./store.js";

const setSlot = (mode, name, value) => {
  results.value = { ...results.value, [mode]: { ...results.value[mode], [name]: value } };
};

/** Starts a run of `mode`: clears its slots and marks `names` as running. */
export function begin(mode, names) {
  results.value = { ...results.value, [mode]: Object.fromEntries(names.map((n) => [n, { state: "running" }])) };
  runs.value = { ...runs.value, [mode]: { started: performance.now(), running: true } };
}
export function end(mode) {
  const r = runs.value[mode];
  if (r) runs.value = { ...runs.value, [mode]: { ...r, running: false, wall: performance.now() - r.started } };
}

/** Awaits one request into slot `name`; returns its data, or null if it failed (the card shows why). */
export async function fill(mode, name, promise) {
  setSlot(mode, name, { state: "running" });
  try {
    const data = await promise;
    setSlot(mode, name, { state: "ok", data });
    return data;
  } catch (err) {
    setSlot(mode, name, { state: "error", error: err?.message ?? String(err) });
    return null;
  }
}
export const drop = (mode, name) => {
  const { [name]: _, ...rest } = results.value[mode] ?? {};
  results.value = { ...results.value, [mode]: rest };
};
export const put = (mode, name, data) => setSlot(mode, name, { state: "ok", data });
export const fail = (mode, name, error) => setSlot(mode, name, { state: "error", error });
