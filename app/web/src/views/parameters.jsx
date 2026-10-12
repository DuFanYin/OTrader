// A strategy's parameters, edited in a dialog (the sidebar shows the changed ones): one number each, at its default until changed, within its declared range
// (the engine rejects a value outside it). Only the changed values are kept and sent.
import { parametersOf } from "../lib/catalog.js";
import { params, setParam } from "../lib/store.js";
import { Line, Num, SmallButton } from "../ui/controls.jsx";
import { DialogGroup } from "../ui/dialog.jsx";

/** The values changed for strategy class `cls`, kept in the input store under `storeKey`. */
export const changedOf = (storeKey, cls) => params.value[storeKey]?.[cls] ?? {};

export function Parameters({ storeKey, cls }) {
  const declared = parametersOf(cls);
  const changed = changedOf(storeKey, cls);
  const keep = (next) => setParam(storeKey, { ...params.value[storeKey], [cls]: next });
  const set = (p, value) => {
    const { [p.name]: _, ...rest } = changed;
    keep(value == null || value === p.default ? rest : { ...rest, [p.name]: value });
  };
  const summary = !cls ? "Choose a strategy" : !declared.length ? "No parameters" : describe(changed);
  return (
    <DialogGroup title={cls ? `${cls} parameters` : "Parameters"} summary={summary}>
      {Object.keys(changed).length > 0 && (
        <div class="col-span-2 flex justify-end">
          <SmallButton title="Put every parameter back to its default" onClick={() => keep({})}>All to defaults</SmallButton>
        </div>
      )}
      {!cls ? <Line>Choose a strategy.</Line> : !declared.length ? <Line>No parameters.</Line> : declared.map((p) => (
        <Num label={p.name} value={changed[p.name] ?? p.default} onChange={(v) => set(p, v)}
          min={p.min} max={p.max} step={p.integer ? 1 : "any"}
          hint={`${p.name}: default ${p.default}, from ${p.min} to ${p.max}${p.integer ? ", whole" : ""}`} />
      ))}
    </DialogGroup>
  );
}

/** The changed values as "name value" pairs, for a run's summary. */
export const describe = (changed) => (Object.keys(changed).length
  ? Object.entries(changed).map(([k, v]) => `${k} ${v}`).join(", ") : "defaults");
