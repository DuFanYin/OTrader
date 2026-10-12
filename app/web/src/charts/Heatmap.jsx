// A grid of coloured cells with a colour bar (IV surface, Greek surfaces).
import { tick } from "../lib/format.js";
import { Frame, PALETTES, light, ramp, rgb } from "./core.jsx";

/**
 * xs, ys: axis values (columns, rows; rows drawn bottom-up); value(i, j) for column i, row j (null: empty).
 * palette: "sequential" | "diverging"; center: value at the palette's middle (diverging);
 * outline(i, j): a CSS colour (or null) bordering a cell, to mark it; cellFmt: label inside big enough cells.
 */
export function Heatmap({ xs, ys, value, palette = "sequential", center, outline, xFmt = tick, yFmt = tick, cellFmt = tick,
  xLabel, yLabel, caption, aspect = 0.42, maxHeight = 340, legendFmt = tick }) {
  const vals = [];
  xs.forEach((_, i) => ys.forEach((_, j) => { const v = value(i, j); if (Number.isFinite(v)) vals.push(v); }));
  if (!vals.length) return null;
  let lo = Math.min(...vals), hi = Math.max(...vals);
  if (palette === "diverging" && center != null) { const r = Math.max(hi - center, center - lo) || 1; lo = center - r; hi = center + r; }
  const t = (v) => (hi === lo ? 0.5 : (v - lo) / (hi - lo));
  const stops = PALETTES[palette];
  return (
    <Frame height={(w) => Math.min(maxHeight, Math.max(160, Math.round(w * aspect)))} caption={caption}>
      {(w, h) => {
        const x0 = 46, x1 = w - 58, y0 = 6, y1 = h - (xLabel ? 32 : 20);
        const cw = (x1 - x0) / xs.length, ch = (y1 - y0) / ys.length;
        const xEvery = Math.ceil(xs.length / Math.max(2, Math.floor((x1 - x0) / 56)));
        const yEvery = Math.ceil(ys.length / Math.max(2, Math.floor((y1 - y0) / 22)));
        return (
          <>
            {xs.map((_, i) => ys.map((_, j) => {
              const v = value(i, j);
              if (!Number.isFinite(v)) return null;
              const c = ramp(stops, t(v)), x = x0 + i * cw, y = y1 - (j + 1) * ch, o = outline?.(i, j);
              return (
                <g>
                  <rect x={x} y={y} width={Math.max(1, cw - 1)} height={Math.max(1, ch - 1)} fill={rgb(c)} rx="1"
                    style={o ? { stroke: o, strokeWidth: 2 } : undefined}>
                    <title>{`${xFmt(xs[i])} · ${yFmt(ys[j])}: ${cellFmt(v)}`}</title>
                  </rect>
                  {cw > 38 && ch > 15 && (
                    <text x={x + cw / 2} y={y + ch / 2 + 3} text-anchor="middle" font-size="9" fill={light(c) ? "#1b1b1f" : "#f4f4f5"}>{cellFmt(v)}</text>
                  )}
                </g>
              );
            }))}
            {xs.map((v, i) => i % xEvery === 0 && <text class="tick" x={x0 + (i + 0.5) * cw} y={y1 + 13} text-anchor="middle">{xFmt(v)}</text>)}
            {ys.map((v, j) => j % yEvery === 0 && <text class="tick" x={x0 - 5} y={y1 - (j + 0.5) * ch + 3} text-anchor="end">{yFmt(v)}</text>)}
            {xLabel && <text class="label" x={(x0 + x1) / 2} y={y1 + 27} text-anchor="middle">{xLabel}</text>}
            {yLabel && <text class="label" transform={`translate(9 ${(y0 + y1) / 2}) rotate(-90)`} text-anchor="middle">{yLabel}</text>}
            <defs>
              <linearGradient id={`bar-${palette}`} x1="0" x2="0" y1="1" y2="0">
                {stops.map((s, k) => <stop offset={`${(k / (stops.length - 1)) * 100}%`} stop-color={rgb(s)} />)}
              </linearGradient>
            </defs>
            <rect x={x1 + 10} y={y0} width="10" height={y1 - y0} rx="2" fill={`url(#bar-${palette})`} />
            <text class="tick" x={x1 + 24} y={y0 + 8}>{legendFmt(hi)}</text>
            <text class="tick" x={x1 + 24} y={y1}>{legendFmt(lo)}</text>
          </>
        );
      }}
    </Frame>
  );
}
