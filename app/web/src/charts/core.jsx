// Chart plumbing: a frame that measures its width and redraws on resize, scales, nice ticks, axes.
// Every chart is SVG, sized in real pixels (text stays 10px at any width), coloured by classes in
// styles.css so it follows the theme.
import { useLayoutEffect, useRef, useState } from "preact/hooks";
import { tick } from "../lib/format.js";

export function useWidth() {
  const ref = useRef(null);
  const [w, setW] = useState(0);
  useLayoutEffect(() => {
    const el = ref.current;
    if (!el) return;
    setW(el.clientWidth);
    const ro = new ResizeObserver(([e]) => setW(Math.round(e.contentRect.width)));
    ro.observe(el);
    return () => ro.disconnect();
  }, []);
  return [ref, w];
}

/** A chart: `children(width)` returns the SVG's content; `height` may depend on the width. */
export function Frame({ height, children, caption, legend, onLeave, onMove }) {
  const [ref, w] = useWidth();
  const h = typeof height === "function" ? height(w) : height;
  return (
    <figure class="chart m-0 flex flex-col gap-1.5">
      {legend}
      <div ref={ref} class="w-full" style={{ height: `${h}px` }}>
        {w > 0 && (
          <svg width={w} height={h} onMouseMove={onMove} onMouseLeave={onLeave} role="img">
            {children(w, h)}
          </svg>
        )}
      </div>
      {caption && <figcaption class="text-xs text-muted">{caption}</figcaption>}
    </figure>
  );
}

export const scale = (d0, d1, r0, r1) => {
  const k = d1 === d0 ? 0 : (r1 - r0) / (d1 - d0);
  return (v) => r0 + (v - d0) * k;
};

/** About `n` round tick values covering [lo, hi]. */
export function ticks(lo, hi, n = 5) {
  if (!Number.isFinite(lo) || !Number.isFinite(hi)) return [];
  if (lo === hi) return [lo];
  const raw = (hi - lo) / Math.max(1, n);
  const mag = 10 ** Math.floor(Math.log10(raw));
  const step = [1, 2, 2.5, 5, 10].map((m) => m * mag).find((s) => s >= raw) ?? raw;
  const out = [];
  for (let v = Math.ceil(lo / step) * step; v <= hi + step * 1e-9; v += step) out.push(Math.abs(v) < step * 1e-9 ? 0 : v);
  return out;
}

/** [lo, hi] of values, padded, optionally including 0. */
export function extent(values, { pad = 0.06, zero = false } = {}) {
  const v = values.filter(Number.isFinite);
  if (!v.length) return [0, 1];
  let lo = Math.min(...v, ...(zero ? [0] : [])), hi = Math.max(...v, ...(zero ? [0] : []));
  if (lo === hi) { lo -= Math.abs(lo) * 0.1 || 1; hi += Math.abs(hi) * 0.1 || 1; }
  const p = (hi - lo) * pad;
  return [zero && lo === 0 ? 0 : lo - p, zero && hi === 0 ? 0 : hi + p];
}

/** Gridlines and tick labels for a plot area [x0, x1] × [y0, y1]. */
export function Axes({ x0, x1, y0, y1, xs, ys, xTicks = [], yTicks = [], xFmt = tick, yFmt = tick, xLabel, yLabel }) {
  return (
    <g>
      {yTicks.map((v) => (
        <g>
          <line class="grid" x1={x0} x2={x1} y1={ys(v)} y2={ys(v)} />
          <text class="tick" x={x0 - 6} y={ys(v) + 3} text-anchor="end">{yFmt(v)}</text>
        </g>
      ))}
      <line class="axis" x1={x0} x2={x1} y1={y1} y2={y1} />
      {xTicks.map((t) => {
        const v = typeof t === "object" ? t.x : t;
        const label = typeof t === "object" ? t.label : xFmt(v);
        return <text class="tick" x={xs(v)} y={y1 + 14} text-anchor="middle">{label}</text>;
      })}
      {xLabel && <text class="label" x={(x0 + x1) / 2} y={y1 + 28} text-anchor="middle">{xLabel}</text>}
      {yLabel && <text class="label" transform={`translate(10 ${(y0 + y1) / 2}) rotate(-90)`} text-anchor="middle">{yLabel}</text>}
    </g>
  );
}

/** A row of coloured keys for multi-series charts. items: [label, cls][] */
export const Legend = ({ items }) => (
  <div class="flex flex-wrap gap-x-3 gap-y-1 text-xs text-muted">
    {items.map(([label, cls, dashed]) => (
      <span class="inline-flex items-center gap-1.5 whitespace-nowrap">
        <svg width="14" height="8" class="chart"><line class={`${cls} line`} x1="1" x2="13" y1="4" y2="4" stroke-dasharray={dashed ? "3 2" : undefined} /></svg>
        {label}
      </span>
    ))}
  </div>
);

/** Interpolated colour from stops ([r,g,b][]) at t in [0, 1]. */
export function ramp(stops, t) {
  const x = Math.max(0, Math.min(1, t)) * (stops.length - 1);
  const i = Math.min(stops.length - 2, Math.floor(x)), f = x - i;
  const [a, b] = [stops[i], stops[i + 1]];
  return a.map((c, k) => Math.round(c + (b[k] - c) * f));
}
export const rgb = ([r, g, b]) => `rgb(${r},${g},${b})`;
export const light = ([r, g, b]) => 0.299 * r + 0.587 * g + 0.114 * b > 150;

export const PALETTES = {
  // low -> high, readable on light and dark pages
  sequential: [[38, 44, 102], [51, 82, 214], [47, 165, 154], [150, 205, 90], [240, 205, 70]],
  diverging: [[51, 92, 200], [140, 170, 230], [232, 232, 226], [235, 150, 120], [200, 60, 50]],
};
