// A modal window for inputs too big for the sidebar (legs, filters): the native <dialog>, so Escape,
// focus and the backdrop come for free. Clicking the backdrop closes it.
import { useEffect, useRef, useState } from "preact/hooks";
import { Button } from "./blocks.jsx";
import { Group } from "./controls.jsx";

export function Dialog({ open, onClose, title, children, footer, width = "640px" }) {
  const ref = useRef(null);
  useEffect(() => {
    const d = ref.current;
    if (open && !d.open) d.showModal();
    if (!open && d.open) d.close();
  }, [open]);
  return (
    <dialog ref={ref} onClose={onClose} onClick={(e) => { if (e.target === ref.current) onClose(); }}
      class="m-auto max-h-[88vh] w-[calc(100vw-24px)] rounded-md border border-line bg-surface p-0 text-ink shadow-2xl backdrop:bg-[var(--overlay)]"
      style={{ maxWidth: width }}>
      {open && (
        <div class="flex max-h-[88vh] flex-col">
          <header class="flex h-11 shrink-0 items-center justify-between border-b border-line px-4">
            <h2 class="text-sm font-semibold">{title}</h2>
            <button type="button" class="text-muted hover:text-ink" onClick={onClose} aria-label="Close">✕</button>
          </header>
          <div class="min-h-0 flex-1 overflow-auto p-4">{children}</div>
          {footer && <footer class="flex shrink-0 items-center justify-end gap-2 border-t border-line px-4 py-3">{footer}</footer>}
        </div>
      )}
    </dialog>
  );
}

export { Button };

/** A summary in the sidebar with the button that opens the full editor. */
export function Summary({ label, children, action, onOpen }) {
  return (
    <div class="col-span-full flex flex-col gap-1">
      {label && <span class="text-xs text-muted">{label}</span>}
      <button type="button" onClick={onOpen}
        class="group flex min-h-[26px] w-full items-start justify-between gap-2 rounded-sm border border-line bg-surface px-2 py-1 text-left text-sm transition-colors hover:border-accent">
        <span class="min-w-0 flex-1">{children}</span>
        <span class="shrink-0 text-xs text-accent">{action}</span>
      </button>
    </div>
  );
}

/** A group of inputs too many or too detailed for the sidebar: the sidebar shows a one-line summary of their values,
 * and Edit opens them in a dialog, a row each (label left, control right) in `columns` columns. */
export function DialogGroup({ title, summary, children, width = "600px", columns = 2 }) {
  const [open, setOpen] = useState(false);
  return (
    <Group title={title}>
      <Summary action="Edit" onOpen={() => setOpen(true)}><span class="text-xs text-ink-2">{summary}</span></Summary>
      <Dialog open={open} onClose={() => setOpen(false)} title={title} width={width}
        footer={<Button primary onClick={() => setOpen(false)}>Done</Button>}>
        <div class="grid gap-x-8 gap-y-1.5" style={{ gridTemplateColumns: `repeat(${columns}, minmax(0, 1fr))` }}>{children}</div>
      </Dialog>
    </Group>
  );
}
