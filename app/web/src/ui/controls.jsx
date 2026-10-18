// The sidebar's inputs. A control bound to the input store (lib/store.js) takes the input's key `k`, and its
// `onChange`, if given, hears each new value; one that is not bound (a strategy parameter) takes `value` and
// `onChange`. Each is one row, its label on the left and the control on the right; one that needs the width (a text
// area, pills, a slider) has its label above.
import { useEffect, useRef, useState } from "preact/hooks";
import { params, setParam } from "../lib/store.js";

const box = "h-[26px] w-full min-w-0 rounded-sm border border-line bg-surface px-2 text-sm text-ink outline-none "
  + "transition-colors hover:border-line-strong focus:border-accent disabled:cursor-not-allowed disabled:opacity-50";

const bind = (k, value, onChange) => (k === undefined
  ? [value, onChange]
  : [params.value[k], (v) => { setParam(k, v); onChange?.(v); }]);

/** A labelled control, a row: a narrow control (a number) keeps a fixed width (`width`), a wide one (a choice of
 * several) takes what the label leaves and the whole row of a dialog's grid; `stack` puts the label above, and one
 * without a label has the row to itself. */
export function Field({ label, hint, wide, stack, width = "w-28", children }) {
  if (!label) return <label class="col-span-full min-w-0" title={hint}>{children}</label>;  // no label: the whole row
  if (stack) {
    return (
      <label class="col-span-full flex min-w-0 flex-col gap-1" title={hint}>
        <span class="truncate text-xs text-muted">{label}</span>
        {children}
      </label>
    );
  }
  return (
    <label class={`flex min-h-[26px] min-w-0 items-center justify-between gap-3 ${wide ? "col-span-full" : ""}`} title={hint}>
      <span class="min-w-0 truncate text-xs text-ink-2">{label}</span>
      <span class={wide ? "min-w-0 max-w-[72%] flex-1" : `${width} shrink-0`}>{children}</span>
    </label>
  );
}

/** A number; empty means null. The text is kept while typing ("0.", "-"), the value is the number. A read-only
 * one with `decimals` shows exactly that many (a live value, so its digits do not jump as it updates). */
export function Num({ label, hint, wide, ...input }) {
  return <Field label={label} hint={hint} wide={wide}><NumInput {...input} /></Field>;
}

/** Num's input alone, for a cell of a table of inputs. */
export function NumInput({ k, value: v, onChange, step = "any", min, max, placeholder = "–", readOnly, disabled, decimals, title }) {
  const [value, set] = bind(k, v, onChange);
  const [text, setText] = useState(value ?? "");
  useEffect(() => {  // the value changed elsewhere (live data, a reset, another strategy): show it
    if (text === "" ? value != null : Number(text) !== value) setText(value ?? "");
  }, [value]);
  return (
    <input type="number" title={title} step={step} min={min} max={max}
      class={`${box} text-right ${readOnly ? "cursor-default bg-surface-2 text-ink-2" : ""}`}
      placeholder={placeholder} readOnly={readOnly} disabled={disabled} value={readOnly && decimals != null && value != null ? Number(value).toFixed(decimals) : text}
      onInput={(e) => {
        const t = e.currentTarget.value;
        setText(t);
        if (t === "") set(null);
        else if (Number.isFinite(Number(t))) set(Number(t));
      }} />
  );
}

export function Text({ k, value: v, onChange, label, hint, wide = true, placeholder, disabled }) {
  const [value, set] = bind(k, v, onChange);
  return (
    <Field label={label} hint={hint} wide={wide}>
      <input type="text" class={box} placeholder={placeholder} disabled={disabled} value={value ?? ""} spellcheck={false}
        onInput={(e) => set(e.currentTarget.value)} />
    </Field>
  );
}

/** A day (YYYY-MM-DD). */
export function Day({ k, label, hint, wide, min, max, disabled }) {
  return (
    <Field label={label} hint={hint} wide={wide} width="w-36">
      <input type="date" class={box} min={min} max={max} disabled={disabled} value={params.value[k] ?? ""}
        onInput={(e) => setParam(k, e.currentTarget.value)} />
    </Field>
  );
}

export function Area({ k, label, hint, rows = 3, placeholder }) {
  return (
    <Field label={label} hint={hint} stack>
      <textarea rows={rows} placeholder={placeholder} spellcheck={false}
        class={`${box} h-auto resize-y py-1 font-mono text-xs leading-snug`} value={params.value[k] ?? ""}
        onInput={(e) => setParam(k, e.currentTarget.value)} />
    </Field>
  );
}

/** One of a few values, as a row of buttons. options: [value, label, title?][] */
export function Seg({ k, label, hint, options, wide = true, value, onChange }) {
  const current = value ?? params.value[k];
  return (
    <Field label={label} hint={hint} wide={wide}>
      <Segmented options={options} value={current} onChange={onChange ?? ((v) => setParam(k, v))} />
    </Field>
  );
}

/** Up to four options in one row; more in two even rows. */
export function Segmented({ options, value, onChange, size = "sm" }) {
  const columns = options.length <= 4 ? options.length : Math.ceil(options.length / 2);
  return (
    <div class="grid min-w-0 gap-px rounded-sm bg-line p-px" role="group" style={{ gridTemplateColumns: `repeat(${columns}, minmax(0, 1fr))` }}>
      {options.map(([v, text, tip]) => (
        <button type="button" title={tip} aria-pressed={v === value} onClick={() => onChange(v)}
          class={`truncate whitespace-nowrap rounded-[3px] px-2 ${size === "xs" ? "h-[22px] text-xs" : "h-[24px] text-sm"} transition-colors `
            + (v === value ? "bg-surface font-medium text-ink shadow-sm" : "bg-surface-2 text-muted hover:text-ink")}>
          {text}
        </button>
      ))}
    </div>
  );
}

/** On / off. */
export function Toggle({ k, label, hint, wide }) {
  const on = !!params.value[k];
  return (
    <Field label={label} hint={hint} wide={wide}>
      <button type="button" role="switch" aria-checked={on} onClick={() => setParam(k, !on)}
        class={`flex h-[26px] items-center gap-2 rounded-sm border px-2 text-sm transition-colors `
          + (on ? "border-accent bg-accent-soft text-ink" : "border-line bg-surface text-muted hover:border-line-strong")}>
        <span class={`h-2 w-2 rounded-full ${on ? "bg-accent" : "bg-line-strong"}`} />{on ? "On" : "Off"}
      </button>
    </Field>
  );
}

/** A set of on / off pills (strategies, checks). items: [key, label, title?][] */
export function Chips({ label, hint, items }) {
  return (
    <Field label={label} hint={hint} stack>
      <div class="flex flex-wrap gap-1">
        {items.map(([k, text, tip]) => {
          const on = !!params.value[k];
          return (
            <button type="button" title={tip} aria-pressed={on} onClick={() => setParam(k, !on)}
              class={`h-[24px] rounded-full border px-2.5 text-xs transition-colors `
                + (on ? "border-accent bg-accent-soft font-medium text-accent" : "border-line bg-surface text-muted hover:text-ink")}>
              {text}
            </button>
          );
        })}
      </div>
    </Field>
  );
}

export function Slider({ k, label, min, max, unit = "" }) {
  const v = params.value[k];
  return (
    <Field label={<span class="flex justify-between"><span>{label}</span><span class="text-ink-2">{v}{unit}</span></span>} stack>
      <input type="range" class="h-[18px] w-full accent-[var(--accent)]" min={min} max={max} step="1" value={v}
        onInput={(e) => setParam(k, Number(e.currentTarget.value))} />
    </Field>
  );
}

/** options: [value, label][]; `placeholder` is shown when nothing is chosen (or there is nothing to choose). */
export function Select({ k, value: v, onChange, label, hint, options, wide = true, placeholder, disabled }) {
  const [value, set] = bind(k, v, onChange);
  return (
    <Field label={label} hint={hint} wide={wide}>
      <Dropdown value={value ?? ""} onChange={set} options={options} placeholder={placeholder} disabled={disabled} />
    </Field>
  );
}

/** A choice from a list, drawn by the page: the chosen one in a box like the other inputs, the list under it (above
 * when there is no room below), fixed to the window so a scrolling sidebar or dialog does not clip it. Arrow keys
 * move, Enter or a click chooses; Escape, a click elsewhere or a scroll closes. */
export function Dropdown({ value, onChange, options, placeholder, disabled, size = "sm" }) {
  const [open, setOpen] = useState(false);
  const [at, setAt] = useState(0);
  const [place, setPlace] = useState({});
  const ref = useRef(null);
  const chosen = options.find(([o]) => String(o) === String(value));
  useEffect(() => {
    if (!open) return;
    const away = (e) => { if (!ref.current?.contains(e.target)) setOpen(false); };
    const scrolled = (e) => { if (!ref.current?.contains(e.target)) setOpen(false); };
    document.addEventListener("mousedown", away);
    document.addEventListener("scroll", scrolled, true);
    return () => { document.removeEventListener("mousedown", away); document.removeEventListener("scroll", scrolled, true); };
  }, [open]);
  const show = () => {
    const r = ref.current.getBoundingClientRect(), below = window.innerHeight - r.bottom;
    setPlace(below < 240 && r.top > below
      ? { left: r.left, minWidth: r.width, bottom: window.innerHeight - r.top + 4 } : { left: r.left, minWidth: r.width, top: r.bottom + 4 });
    setAt(Math.max(0, options.findIndex(([o]) => String(o) === String(value))));
    setOpen(true);
  };
  const choose = (i) => { setOpen(false); if (options[i] && String(options[i][0]) !== String(value)) onChange(options[i][0]); };
  const key = (e) => {
    if (e.key === "Escape" && open) { e.preventDefault(); e.stopPropagation(); setOpen(false); }
    else if (e.key === "ArrowDown" || e.key === "ArrowUp") {
      e.preventDefault();
      if (!open) show();
      else setAt((i) => Math.min(options.length - 1, Math.max(0, i + (e.key === "ArrowDown" ? 1 : -1))));
    } else if ((e.key === "Enter" || e.key === " ") && open) { e.preventDefault(); choose(at); }
  };
  const height = size === "xs" ? "h-6 text-xs" : "h-[26px] text-sm";
  return (
    <span ref={ref} class="relative block min-w-0">
      <button type="button" disabled={disabled || !options.length} onClick={() => (open ? setOpen(false) : show())} onKeyDown={key}
        aria-haspopup="listbox" aria-expanded={open}
        class={`${box} ${height} flex items-center justify-between gap-2 text-left ${open ? "border-accent" : ""}`}>
        <span class={`truncate ${chosen ? "" : "text-muted"}`}>{chosen ? chosen[1] : (placeholder ?? "–")}</span>
        <svg viewBox="0 0 10 10" class={`h-2.5 w-2.5 shrink-0 text-muted transition-transform ${open ? "rotate-180" : ""}`} fill="none"
          stroke="currentColor" stroke-width="1.5" aria-hidden="true"><path d="M2 3.75 5 6.75l3-3" /></svg>
      </button>
      {open && (
        <ul role="listbox" style={place} class="fixed z-50 max-h-60 max-w-[90vw] overflow-auto rounded-sm border border-line-strong bg-surface p-1 shadow-xl">
          {options.map(([o, text], i) => (
            <li role="option" aria-selected={String(o) === String(value)} onMouseDown={(e) => { e.preventDefault(); choose(i); }}
              onMouseEnter={() => setAt(i)}
              class={`flex cursor-pointer items-center justify-between gap-3 rounded-[3px] px-2 py-1 ${size === "xs" ? "text-xs" : "text-sm"} `
                + `${i === at ? "bg-surface-2 text-ink" : "text-ink-2"}`}>
              <span class="truncate">{text}</span>
              {String(o) === String(value) && <span class="text-xs text-accent">✓</span>}
            </li>
          ))}
        </ul>
      )}
    </span>
  );
}

/** A group of controls in the sidebar, under a small heading (with an optional action on its right). */
export function Group({ title, action, children }) {
  return (
    <section class="border-b border-line px-3 py-3 last:border-b-0">
      <div class="mb-2 flex h-5 items-center justify-between">
        <h3 class="text-2xs font-semibold tracking-[0.08em] text-muted uppercase">{title}</h3>
        {action}
      </div>
      <div class="grid grid-cols-1 gap-y-1.5">{children}</div>
    </section>
  );
}

export function SmallButton({ children, onClick, title, disabled }) {
  return (
    <button type="button" title={title} disabled={disabled} onClick={onClick}
      class="h-5 rounded-sm px-1.5 text-xs text-muted transition-colors hover:bg-surface-2 hover:text-ink disabled:opacity-40">
      {children}
    </button>
  );
}

/** A line of text across the group's two columns. */
export const Line = ({ children }) => <p class="col-span-full text-xs leading-relaxed text-muted">{children}</p>;
