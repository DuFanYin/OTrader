// A strategy's parameters, and a backtest's costs, edited in a dialog (the sidebar shows the changed ones): each at
// its default until changed, a number within its declared range (the engine rejects a value outside it) or one of
// its options. Only the changed values are kept and sent. The dialog lays them out by their names: numbered ones
// (leg1_ratio … leg4_dte) as a table, a row a number; a pair x_min, x_max as one range; a whole number from 0 to 1
// or a choice as a switch; the rest a row each.
import { costsDeclared, parametersOf } from "../lib/catalog.js";
import { params, setParam } from "../lib/store.js";
import { Field, Line, NumInput, Segmented, SmallButton } from "../ui/controls.jsx";
import { DialogGroup } from "../ui/dialog.jsx";

/** The values changed for strategy class `cls`, kept in the input store under `storeKey`. */
export const changedOf = (storeKey, cls) => params.value[storeKey]?.[cls] ?? {};

/** The declared parameters in order, numbered ones gathered into tables and min/max pairs into ranges. */
function layout(declared) {
  const out = [], tables = {}, byName = Object.fromEntries(declared.map((p) => [p.name, p]));
  for (const p of declared) {
    const numbered = p.name.match(/^([a-z]+)(\d+)_(.+)$/);
    if (numbered) {
      const [, stem, n, column] = numbered;
      let t = tables[stem];
      if (!t) out.push(t = tables[stem] = { kind: "table", stem, rows: [], columns: [], cells: {} });
      if (!t.rows.includes(n)) t.rows.push(n);
      if (!t.columns.includes(column)) t.columns.push(column);
      t.cells[`${n}:${column}`] = p;
    } else if (p.name.endsWith("_max") && byName[`${p.name.slice(0, -4)}_min`]) {
      continue;  // with its _min
    } else if (p.name.endsWith("_min") && byName[`${p.name.slice(0, -4)}_max`]) {
      out.push({ kind: "range", name: p.name.slice(0, -4), lo: p, hi: byName[`${p.name.slice(0, -4)}_max`] });
    } else {
      out.push({ kind: "one", p });
    }
  }
  return out;
}

const range = (p) => [p.min != null && `from ${p.min}`, p.max != null && `to ${p.max}`].filter(Boolean).join(" ");
const hint = (p) => [`${p.name}: default ${p.default}${range(p) ? `, ${range(p)}` : ""}${p.integer ? ", whole" : ""}`,
  p.description].filter(Boolean).join(". ");
const isSwitch = (p) => p.integer && p.min === 0 && p.max === 1;

/** Declared values (a strategy's parameters, the costs) in a dialog; keep(next) stores the changed ones. */
function ParameterDialog({ title, declared, changed, keep, summary, empty, width }) {
  const set = (p, value) => {
    const { [p.name]: _, ...rest } = changed;
    keep(value == null || value === p.default ? rest : { ...rest, [p.name]: value });
  };
  const valueOf = (p) => changed[p.name] ?? p.default;
  const name = (text, ...ps) => <span class={ps.some((p) => p.name in changed) ? "text-accent" : ""}>{text}</span>;
  const input = (p) => (p.options
    ? <Segmented size="xs" options={p.options.map((o) => [o, o])} value={valueOf(p)} onChange={(v) => set(p, v)} />
    : isSwitch(p)
      ? <Segmented size="xs" options={[[0, "0"], [1, "1"]]} value={valueOf(p)} onChange={(v) => set(p, v)} />
      : <NumInput value={valueOf(p)} onChange={(v) => set(p, v)} min={p.min ?? undefined} max={p.max ?? undefined}
          step={p.integer ? 1 : "any"} title={hint(p)} />);
  return (
    <DialogGroup title={title} summary={summary} width={width}>
      {Object.keys(changed).length > 0 && (
        <div class="col-span-full flex justify-end">
          <SmallButton title="Put every value back to its default" onClick={() => keep({})}>All to defaults</SmallButton>
        </div>
      )}
      {empty ? <Line>{empty}</Line> : layout(declared).map((item) => {
        if (item.kind === "one") return <Field label={name(item.p.name, item.p)} hint={hint(item.p)}>{input(item.p)}</Field>;
        if (item.kind === "range") {
          return (
            <Field label={name(item.name, item.lo, item.hi)} hint={`${hint(item.lo)}; ${hint(item.hi)}`}>
              <span class="flex items-center gap-1">{input(item.lo)}<span class="text-xs text-muted">–</span>{input(item.hi)}</span>
            </Field>
          );
        }
        const { stem, rows, columns, cells } = item;
        return (
          <table class="col-span-full my-1 w-full table-fixed border-separate border-spacing-x-1.5 border-spacing-y-1 text-xs">
            <thead>
              <tr>
                <th class="w-14" />
                {columns.map((c) => <th class="px-0.5 text-right font-normal text-muted">{c}</th>)}
              </tr>
            </thead>
            <tbody>
              {rows.map((n) => (
                <tr>
                  <td class="w-14 whitespace-nowrap text-ink-2">{name(`${stem} ${n}`, ...columns.map((c) => cells[`${n}:${c}`]).filter(Boolean))}</td>
                  {columns.map((c) => <td>{cells[`${n}:${c}`] && input(cells[`${n}:${c}`])}</td>)}
                </tr>
              ))}
            </tbody>
          </table>
        );
      })}
    </DialogGroup>
  );
}

export function Parameters({ storeKey, cls }) {
  const declared = parametersOf(cls);
  const changed = changedOf(storeKey, cls);
  const keep = (next) => setParam(storeKey, { ...params.value[storeKey], [cls]: next });
  const summary = !cls ? "Choose a strategy" : !declared.length ? "No parameters" : describe(changed);
  return <ParameterDialog title="Strategy parameters" declared={declared} changed={changed} keep={keep} summary={summary}
    empty={!cls ? "Choose a strategy." : !declared.length ? "No parameters." : null} width="680px" />;
}

/** A backtest's costs, kept under `storeKey` as the changed ones (the server has every default). */
export function Costs({ storeKey }) {
  const changed = params.value[storeKey] ?? {};
  return <ParameterDialog title="Costs" declared={costsDeclared()} changed={changed}
    keep={(next) => setParam(storeKey, next)} summary={describe(changed)} />;
}

/** Of recorded costs, those the dialog has that are not their defaults (an experiment's, opened in Backtest). */
export const changedCosts = (recorded) => Object.fromEntries(costsDeclared()
  .filter((c) => recorded?.[c.name] !== undefined && recorded[c.name] !== c.default).map((c) => [c.name, recorded[c.name]]));

/** The changed values as "name value" pairs, for a run's summary. */
export const describe = (changed) => (Object.keys(changed).length
  ? Object.entries(changed).map(([k, v]) => `${k} ${v}`).join(", ") : "defaults");
