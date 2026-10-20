// What there is to backtest on (data/backtest/<SYMBOL>/... day files, by symbol) and the live engine's contracts.
import { signal } from "@preact/signals";
import { get } from "./api.js";

export const files = signal({ state: "idle", symbols: [] });  // symbols: [{ name, path, number_of_days, date_start, date_end, ... }]
export const contracts = signal({ state: "idle" });           // { equity: { total, rows }, option: { total, chains, rows } }

export async function loadFiles() {
  files.value = { ...files.value, state: "loading" };
  try {
    const list = await get("api/files");
    files.value = { state: "ok", symbols: list.filter((f) => f.type === "segment") };
  } catch (err) {
    files.value = { state: "error", symbols: [], error: err.message };
  }
}

export async function loadContracts() {
  contracts.value = { ...contracts.value, state: "loading" };
  try {
    contracts.value = { state: "ok", ...(await get("api/database/contracts")) };
  } catch (err) {
    contracts.value = { state: "error", error: err.message };
  }
}
