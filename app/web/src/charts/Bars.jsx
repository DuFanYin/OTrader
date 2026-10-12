// Bar charts: horizontal bars (tornado, rankings, thresholds), small multiples, columns, histogram.
import { tick } from "../lib/format.js";
import { Axes, Frame, extent, scale, ticks } from "./core.jsx";

const ROW = 20;

/**
 * Horizontal bars, one row each. rows: [{ label, value, cls?, ref?, text? }]; text replaces the value's label.
 * signed: bars grow both ways from 0 (tornado); ref: a faint bar behind, e.g. a threshold; max: a fixed end of the scale.
 */
export function HBars({ rows, signed, fmt = tick, caption, labelWidth = 112, max: top }) {
  const data = rows.filter((r) => Number.isFinite(Number(r.value)));
  if (!data.length) return null;
  const max = top ?? Math.max(...data.flatMap((r) => [Math.abs(r.value), Math.abs(r.ref ?? 0)]), 1e-12);
  return (
    <Frame height={data.length * ROW + 6} caption={caption}>
      {(w) => {
        const x0 = Math.min(labelWidth, w * 0.4), x1 = w - 52;
        const zx = signed ? (x0 + x1) / 2 : x0;
        const len = scale(0, max, 0, signed ? (x1 - x0) / 2 - 40 : x1 - x0);  // signed: room for the value beside a bar
        return (
          <>
            <line class="axis" x1={zx} x2={zx} y1="0" y2={data.length * ROW} />
            {data.map((r, i) => {
              const y = i * ROW + 3, v = Number(r.value), bw = len(Math.abs(v));
              const x = v >= 0 ? zx : zx - bw;
              const cls = r.cls ?? (signed ? (v >= 0 ? "up" : "down") : "s1");
              return (
                <g>
                  <text class="tick" x={x0 - 6} y={y + 11} text-anchor="end">
                    <title>{r.label}</title>{String(r.label).length > 18 ? String(r.label).slice(0, 17) + "…" : r.label}
                  </text>
                  {r.ref != null && <rect class="muted" x={zx} y={y} width={len(Math.abs(r.ref))} height="14" rx="2" opacity="0.5" />}
                  <rect class={cls} x={x} y={y + (r.ref != null ? 3 : 1)} width={Math.max(bw, 1)} height={r.ref != null ? 8 : 12} rx="2" opacity="0.85" />
                  <text class="value" x={v >= 0 ? x + bw + 4 : x - 4} y={y + 11} text-anchor={v >= 0 ? "start" : "end"}>{r.text ?? fmt(v)}</text>
                </g>
              );
            })}
          </>
        );
      }}
    </Frame>
  );
}

/** One small bar chart per key, each with its own scale: values of different kinds side by side. */
export function Multiples({ rows, keys, labelKey, fmt = tick }) {
  return (
    <div class="grid gap-3 sm:grid-cols-2">
      {keys.map((k) => (
        <div class="min-w-0">
          <p class="mb-1 text-xs text-muted">{k}</p>
          <HBars rows={rows.map((r) => ({ label: r[labelKey], value: Number(r[k]) }))} fmt={fmt} labelWidth={84} />
        </div>
      ))}
    </div>
  );
}

/** Vertical columns over an index (calibration residuals); signed values go up / down from 0. */
export function Columns({ values, height = 120, caption, labels }) {
  const v = values.map(Number);
  if (!v.length) return null;
  const [lo, hi] = extent(v, { zero: true, pad: 0.1 });
  return (
    <Frame height={height} caption={caption}>
      {(w, h) => {
        const x0 = 46, x1 = w - 8, y0 = 8, y1 = h - 18;
        const ys = scale(lo, hi, y1, y0), bw = (x1 - x0) / v.length;
        return (
          <>
            <Axes x0={x0} x1={x1} y0={y0} y1={y1} xs={(i) => x0 + (i + 0.5) * bw} ys={ys} yTicks={ticks(lo, hi, 3)}
              xTicks={v.map((_, i) => i).filter((i) => v.length <= 16 || i % 2 === 0).map((i) => ({ x: i, label: labels?.[i] ?? i + 1 }))} />
            <line class="axis" x1={x0} x2={x1} y1={ys(0)} y2={ys(0)} />
            {v.map((x, i) => (
              <rect class={x >= 0 ? "down" : "s1"} x={x0 + i * bw + bw * 0.2} width={bw * 0.6} rx="1.5" opacity="0.85"
                y={Math.min(ys(x), ys(0))} height={Math.max(1, Math.abs(ys(x) - ys(0)))}><title>{tick(x)}</title></rect>
            ))}
          </>
        );
      }}
    </Frame>
  );
}

/** A histogram from bin edges and counts; bins below 0 in the loss colour; marks: [{x, label}]. */
export function Histogram({ edges, counts, marks = [], height = 150, caption, xLabel }) {
  if (!edges?.length || !counts?.length) return null;
  const lo = edges[0], hi = edges[counts.length], max = Math.max(...counts, 1);
  return (
    <Frame height={height} caption={caption}>
      {(w, h) => {
        const x0 = 40, x1 = w - 8, y0 = 12, y1 = h - (xLabel ? 32 : 20);
        const xs = scale(lo, hi, x0, x1), ys = scale(0, max, y1, y0);
        return (
          <>
            <Axes x0={x0} x1={x1} y0={y0} y1={y1} xs={xs} ys={ys} xLabel={xLabel}
              xTicks={ticks(lo, hi, Math.max(2, Math.floor((x1 - x0) / 80)))} yTicks={ticks(0, max, 3)} />
            {counts.map((c, i) => {
              const a = xs(edges[i]), b = xs(edges[i + 1]);
              return <rect class={(edges[i] + edges[i + 1]) / 2 < 0 ? "down" : "up"} opacity="0.7" x={a + 0.5} width={Math.max(1, b - a - 1)} y={ys(c)} height={y1 - ys(c)} />;
            })}
            {lo < 0 && hi > 0 && <line class="axis" x1={xs(0)} x2={xs(0)} y1={y0} y2={y1} />}
            {marks.filter((m) => m.x != null && m.x >= lo && m.x <= hi).map((m) => (
              <g>
                <line class="zero" x1={xs(m.x)} x2={xs(m.x)} y1={y0} y2={y1} />
                <text class="label" x={xs(m.x)} y={y0 - 2} text-anchor="middle">{m.label}</text>
              </g>
            ))}
          </>
        );
      }}
    </Frame>
  );
}
