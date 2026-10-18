// Live market data (Live mode): spot, DVOL, funding and the Treasury curve fill the read-only inputs,
// then the server interpolates sigma and r for the current strike and maturity from the IV surface.
import { signal } from "@preact/signals";
import { get, post } from "./api.js";
import { params, setParams, settings } from "./store.js";

export const market = signal({ state: "idle" });
const patch = (m) => { market.value = { ...market.value, ...m }; };

/** A fraction as a percent, rounded to `digits`. */
const pct = (x, digits) => Number((100 * x).toFixed(digits));
/** A value the server could not fetch and gave a fixed one for: named in market.fallback. */
const noteFallback = (name, source) => {
  if (source === "fallback") patch({ fallback: [...(market.value.fallback ?? []), name] });
};

let snap = null;          // the last fetch: spot, rates, surface (for re-resolving without fetching again)
let strikeEdited = false; // the strike follows spot until the user types one
export const markStrikeEdited = () => { strikeEdited = true; };
const live = () => settings.value.dataMode === "live";

const FRESH_MS = 60_000;  // a view switch fetches again only when the last fetch is older than this
let fetchedAt = 0;

/** Fetches the market unless the last fetch is fresh (force: the refresh button, the switch to Live). */
export async function fetchLive({ force = false } = {}) {
  if (!live()) return;
  if (!force && snap && Date.now() - fetchedAt < FRESH_MS) return;
  fetchedAt = Date.now();
  const s = { rate_curve: {}, iv_surface: [] };
  snap = s;
  const current = () => live() && snap === s;  // a newer fetch or a switch to Sim makes this one moot
  patch({ state: "loading", error: null });

  const spot = get("api/lab/market/spot").then((d) => {
    s.spot = d.spot;
    if (current()) {
      setParams(strikeEdited ? { spot: d.spot } : { spot: d.spot, strike: d.spot });
      patch({ spot: d.spot, fallback: d.source === "fallback" ? ["spot"] : [] });
    }
    return d.spot;
  });
  get("api/lab/market/dvol").then((d) => {
    s.vol = d.vol;
    if (current()) { setParams({ vol: d.vol }); patch({ ivPct: pct(d.vol, 2) }); noteFallback("σ", d.source); }
  }).catch(() => {});
  get("api/lab/market/funding").then((d) => {
    if (current()) { setParams({ mu: d.mu }); patch({ muPct: pct(d.mu, 3), funding8h: pct(d.funding_8h, 4) }); noteFallback("μ", d.source); }
  }).catch(() => {});
  const rates = get("api/lab/market/rates").then((d) => {
    Object.assign(s, { rate: d.rate, rate_curve: d.rate_curve });
    if (current()) {
      patch({ ratePct: pct(d.rate, 3), curve: Object.fromEntries(Object.entries(d.rate_curve).map(([k, v]) => [k, pct(v, 3)])) });
      noteFallback("r", d.source);
    }
  }).catch(() => {});
  const surface = spot.then((x) => get(`api/lab/market/surface?spot=${encodeURIComponent(x)}`))
    .then((d) => { s.iv_surface = d.iv_surface; }).catch(() => {});

  Promise.all([surface, rates]).then(() => resolve(s)).catch(() => {});
  try {
    await spot;
    patch({ state: "ok", at: new Date() });
  } catch (err) {
    patch({ state: "error", error: err.message });
    throw err;
  }
}

/** sigma and r for the current strike and maturity, from the last fetch (no new market requests). */
async function resolve(s = snap) {
  if (!s || !live() || !s.spot) return;
  const p = params.value;
  let r = null;
  try {
    r = await post("api/lab/market/resolve", { target_strike: Number(p.strike) || s.spot, target_maturity: Number(p.maturity) || 0.25,
      spot: s.spot, iv_surface: s.iv_surface ?? [], rate_curve: s.rate_curve ?? {} });
  } catch { /* fall back to DVOL and the 3-month rate below */ }
  if (!live() || snap !== s) return;
  if (!r) {
    setParams({ vol: s.vol ?? p.vol, rate: s.rate ?? p.rate });
    patch({ ivSource: "DVOL index (no surface)", ivMatch: "fallback", rateTenor: "3m (fallback)" });
    return;
  }
  setParams({ vol: r.vol, rate: r.rate });
  patch({
    ratePct: pct(r.rate, 4), rateTenor: `${r.rate_tenor} → ${pct(r.rate, 4)}%`,
    ivSource: r.iv_instrument || "DVOL index (no surface)",
    ivMatch: r.iv_instrument ? `K ${Number(r.iv_matched_strike).toLocaleString()} · T ${r.iv_matched_years}y · ${r.interp_method}` : "fallback",
  });
}

let timer = null;
/** Re-resolve after the strike or maturity changed (debounced). */
export function reresolve() {
  clearTimeout(timer);
  timer = setTimeout(() => resolve().catch(() => {}), 250);
}
