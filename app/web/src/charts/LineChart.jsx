// Lines over x (term structure, smile, convergence, paths, payoff, cumulative P&L, Greeks over a backtest), with a hover readout.
import { useState } from "preact/hooks";
import { tick } from "../lib/format.js";
import { Axes, Frame, Legend, extent, scale, ticks } from "./core.jsx";

/**
 * series: [{ label, points: [{x, y, label?}], cls = "s1", area, dots, dashed }]
 * marks: [{ x, label }] dashed verticals; zero: draw y = 0 and include it; highlight: x of a larger dot.
 * xTicks: [{x, label}] instead of computed ones; xFmt / yFmt / valueFmt: tick and readout formats.
 */
export function LineChart({ series, height = 200, xLabel, yLabel, xFmt = tick, yFmt = tick, valueFmt = yFmt,
  marks = [], zero, highlight, xTicks, caption, legend = series.length > 1, pointLabels }) {
  const [hover, setHover] = useState(null);
  const all = series.flatMap((s) => s.points).filter((p) => Number.isFinite(p.x) && Number.isFinite(p.y));
  if (!all.length) return null;
  const [xlo, xhi] = extent(all.map((p) => p.x), { pad: 0 });
  const [ylo, yhi] = extent(all.map((p) => p.y), { zero, pad: 0.1 });
  const pad = { l: 46, r: 12, t: 14, b: xLabel ? 32 : 20 };
  const base = series[0].points;

  return (
    <Frame height={height} caption={caption}
      legend={legend && <Legend items={series.map((s) => [s.label, s.cls ?? "s1", s.dashed])} />}
      onLeave={() => setHover(null)}
      onMove={(e) => {
        const box = e.currentTarget.getBoundingClientRect();
        const px = e.clientX - box.left;
        const xs = scale(xlo, xhi, pad.l, box.width - pad.r);
        let best = null;
        base.forEach((p, i) => { if (best === null || Math.abs(xs(p.x) - px) < Math.abs(xs(base[best].x) - px)) best = i; });
        setHover(best);
      }}>
      {(w, h) => {
        const x0 = pad.l, x1 = w - pad.r, y0 = pad.t, y1 = h - pad.b;
        const xs = scale(xlo, xhi, x0, x1), ys = scale(ylo, yhi, y1, y0);
        const path = (pts) => pts.filter((p) => Number.isFinite(p.y)).map((p, i) => `${i ? "L" : "M"}${xs(p.x).toFixed(1)},${ys(p.y).toFixed(1)}`).join("");
        const hp = hover != null ? base[hover] : null;
        return (
          <>
            <Axes x0={x0} x1={x1} y0={y0} y1={y1} xs={xs} ys={ys} xLabel={xLabel} yLabel={yLabel} xFmt={xFmt} yFmt={yFmt}
              xTicks={xTicks ?? ticks(xlo, xhi, Math.max(2, Math.floor((x1 - x0) / 80)))} yTicks={ticks(ylo, yhi, Math.max(2, Math.floor((y1 - y0) / 34)))} />
            {zero && ylo < 0 && yhi > 0 && <line class="zero" x1={x0} x2={x1} y1={ys(0)} y2={ys(0)} />}
            {marks.filter((m) => m.x >= xlo && m.x <= xhi).map((m) => (
              <g>
                <line class="zero" x1={xs(m.x)} x2={xs(m.x)} y1={y0} y2={y1} />
                <text class="label" x={xs(m.x) + 3} y={y0 + 9}>{m.label}</text>
              </g>
            ))}
            {series.map((s) => {
              const cls = s.cls ?? "s1";
              const pts = s.points.filter((p) => Number.isFinite(p.y));
              if (!pts.length) return null;
              return (
                <g>
                  {s.area && <path class={`${cls} area`} d={`${path(pts)}L${xs(pts.at(-1).x)},${y1}L${xs(pts[0].x)},${y1}Z`} />}
                  <path class={`${cls} line`} d={path(pts)} stroke-dasharray={s.dashed ? "4 3" : undefined} />
                  {s.dots && pts.map((p) => <circle class={`${cls} dot`} cx={xs(p.x)} cy={ys(p.y)} r={p.x === highlight ? 4.5 : 2.5} />)}
                  {pointLabels && pts.map((p) => <text class="value" x={xs(p.x)} y={ys(p.y) - 7} text-anchor="middle">{pointLabels(p)}</text>)}
                </g>
              );
            })}
            {hp && (
              <g pointer-events="none">
                <line class="hover-line" x1={xs(hp.x)} x2={xs(hp.x)} y1={y0} y2={y1} />
                {series.map((s) => {
                  const p = s.points[hover];
                  return p && Number.isFinite(p.y) && <circle class={`${s.cls ?? "s1"} dot`} cx={xs(p.x)} cy={ys(p.y)} r="3.5" />;
                })}
                <text class="value" x={x1} y={y0 - 3} text-anchor="end">
                  {`${hp.label ?? xFmt(hp.x)}  ·  ${series.map((s) => `${series.length > 1 ? s.label + " " : ""}${valueFmt(s.points[hover]?.y)}`).join("  ·  ")}`}
                </text>
              </g>
            )}
          </>
        );
      }}
    </Frame>
  );
}
