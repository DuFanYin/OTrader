// Errors that are not tied to a card (a run that could not start, a refresh that failed).
import { signal } from "@preact/signals";

const toasts = signal([]);
let next = 0;

export function toast(message, title = "Request failed") {
  const id = ++next;
  toasts.value = [...toasts.value.slice(-2), { id, title, message: String(message) }];
  setTimeout(() => dismiss(id), 7000);
}
const dismiss = (id) => { toasts.value = toasts.value.filter((t) => t.id !== id); };

export function Toasts() {
  return (
    <div class="fixed right-3 bottom-3 z-50 flex w-[min(380px,calc(100vw-24px))] flex-col gap-2" aria-live="assertive">
      {toasts.value.map((t) => (
        <div role="alert" class="rounded-md border border-down/40 bg-surface p-3 shadow-lg">
          <div class="flex items-start justify-between gap-3">
            <p class="text-sm font-semibold text-down">{t.title}</p>
            <button type="button" class="text-xs text-muted hover:text-ink" onClick={() => dismiss(t.id)} aria-label="Dismiss">✕</button>
          </div>
          <p class="mt-1 font-mono text-xs leading-relaxed break-words text-ink-2">{t.message}</p>
        </div>
      ))}
    </div>
  );
}
