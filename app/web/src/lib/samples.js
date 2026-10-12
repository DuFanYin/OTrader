// Sample results: every view that computes opens with one (public/samples/sample_<view id>.json, a run made with
// the default inputs), so its results pane is never empty. A view offers sample: { dump, show }: dump gives its
// results to save as a sample (window.optraSample in `npm run dev`), show puts a saved one on screen. The samples
// are fetched once, when the page opens, so switching to a view shows its sample at once; a run replaces it.
import { signal } from "@preact/signals";

const saved = {};                       // view id -> its sample (null: none shipped)
export const loaded = signal(false);    // every sample fetched (or found missing)
export const shown = signal({});        // view id -> the sample's file name, while that sample is on screen

const fileOf = (id) => `sample_${id}.json`;

/** Fetches every view's sample. */
export async function loadSamples(views) {
  await Promise.all(views.filter((v) => v.sample).map(async (v) => {
    try {
      const res = await fetch(new URL(`samples/${fileOf(v.id)}`, document.baseURI).href);
      saved[v.id] = res.ok ? await res.json() : null;
    } catch {
      saved[v.id] = null;
    }
  }));
  loaded.value = true;
}

/** Shows the view's sample when it has no results of its own. */
export function showSample(view) {
  if (!view.sample || !saved[view.id] || view.hasResults()) return;
  view.sample.show(saved[view.id]);
  shown.value = { ...shown.value, [view.id]: fileOf(view.id) };
}

/** The view's own result replaces the sample. */
export function dropSample(id) {
  if (!(id in shown.value)) return;
  const { [id]: _, ...rest } = shown.value;
  shown.value = rest;
}
