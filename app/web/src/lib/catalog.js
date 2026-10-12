// The strategy classes the engine has and their declared parameters (default, min, max, whole number or not),
// from `entry_backtest --list-strategies`. The live engine runs the same classes.
import { signal } from "@preact/signals";
import { get } from "./api.js";

export const catalog = signal({ state: "idle", list: [] });  // list: [{ name, parameters: [...] }]

export async function loadCatalog() {
  catalog.value = { ...catalog.value, state: "loading" };
  try {
    const d = await get("api/backtest/strategies");  // the strategy_classes tool
    catalog.value = { state: "ok", list: d.result_summary?.strategies ?? [] };
  } catch (err) {
    catalog.value = { state: "error", list: [], error: err.message };
  }
}

export const parametersOf = (name) => catalog.value.list.find((s) => s.name === name)?.parameters ?? [];
