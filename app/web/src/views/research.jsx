// A research view (pricing, multi-leg, risk, numerics, screener, validation) as a view of the page: its run
// fetches live market data first, runs the validation gate when asked, keeps its results in named slots
// (lib/runner.js) and records itself in the run history.
import { signal } from "@preact/signals";
import { record, restored } from "../lib/history.js";
import { ms } from "../lib/format.js";
import { fetchLive } from "../lib/market.js";
import { anyCheck } from "../lib/payloads.js";
import { drop, end } from "../lib/runner.js";
import { params, results, runs, settings } from "../lib/store.js";
import { Slot } from "../ui/blocks.jsx";
import { toast } from "../ui/toast.jsx";
import { Report, gate } from "./validation.jsx";

async function run(mode, busy) {
  if (busy.value) return;
  busy.value = true;
  const pre = `pre:${mode.id}`;
  try {
    if (settings.value.dataMode === "live" && mode.id !== "screener") await fetchLive({ force: true }).catch(() => {});
    if (mode.precheck && params.value.precheck && anyCheck(params.value)) {
      const passed = await gate(pre, "gate");
      if (!passed && params.value.blockOnFail) {
        toast("The validation gate failed and Block on fail is on: the run was stopped.", "Blocked by the pre-check");
        return;
      }
    } else {
      drop(pre, "gate");
    }
    await mode.run();
  } finally {
    end(mode.id);
    record(mode.id);
    busy.value = false;
  }
}

function status(id) {
  const r = runs.value[id];
  if (r?.running) return "running…";
  if (restored.value?.mode === id) {
    return `from the history: ${new Date(restored.value.at).toLocaleString([], { dateStyle: "short", timeStyle: "short" })}`;
  }
  return r?.wall != null ? `last run ${ms(r.wall)} wall` : "";
}

/** A research mode ({ id, label, about, Params, Results, run, precheck, enter? }) as a view. */
export function researchView(mode) {
  const busy = signal(false);
  const pre = () => results.value[`pre:${mode.id}`]?.gate;
  return {
    id: mode.id,
    label: mode.label,
    about: mode.about,
    research: true,
    enter: async () => {
      if (settings.value.dataMode === "live") fetchLive().catch(() => {});
      await mode.enter?.();
    },
    Params: mode.Params,
    Results: () => (
      <>
        {pre() && <Slot mode={`pre:${mode.id}`} slot="gate" title="Pre-check" wide>{(d) => <Report data={d} compact />}</Slot>}
        <mode.Results />
      </>
    ),
    hasResults: () => Object.keys(results.value[mode.id] ?? {}).length > 0 || !!pre(),
    status: () => status(mode.id),
    action: { label: `Run ${mode.label}`, busyLabel: "Running…", busy, run: () => run(mode, busy) },
    sample: {
      dump: () => {
        const ok = (slots) => Object.fromEntries(Object.entries(slots ?? {}).filter(([, s]) => s.state === "ok"));
        return { slots: ok(results.value[mode.id]), pre: ok(results.value[`pre:${mode.id}`]) };
      },
      show: ({ slots, pre }) => {
        results.value = { ...results.value, [mode.id]: slots ?? {}, [`pre:${mode.id}`]: pre ?? {} };
      },
    },
  };
}
