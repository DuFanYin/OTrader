// The engine's state, polled from the backend: is the backend up, and the live engine (entry_live), the IB
// gateway and market data.
import { computed, signal } from "@preact/signals";
import { get } from "./api.js";
import { poll } from "./poll.js";

export const status = signal({ state: "loading" });  // { state: "ok", live } | { state: "down", error }
/** A public demo: a simulated account on synthetic days (the server's OPTRA_DEMO). */
export const demo = signal(false);
/** The venue's name on screen: IB, or in the demo the simulated one (no broker behind it). */
export const broker = () => (demo.value ? "Sim" : "IB");
/** The connection to it: "IB gateway", or "Sim gateway". */
export const gatewayName = () => (demo.value ? "Sim gateway" : "IB gateway");

export async function refreshStatus() {
  try {
    const s = await get("api/system/status");
    status.value = { state: "ok", live: s.live ?? {} };
    demo.value = s.demo === true;
  } catch (err) {
    status.value = { state: "down", error: err.message };
  }
}

/** The live engine's parts, read from its status line ("engine: running; ib: on; md: off"; "; trading: halted"). */
export const live = computed(() => {
  const l = status.value.live ?? {};
  const detail = l.detail ?? "";
  return {
    running: l.status === "running", ib: /ib: on/.test(detail), md: /md: on/.test(detail),
    halted: /trading: halted/.test(detail), detail, state: l.status,
  };
});

let watching = false;
export function watchStatus(ms = 3000) {
  if (watching) return;
  watching = true;
  poll(refreshStatus, ms, () => status.value.state === "down");
}
