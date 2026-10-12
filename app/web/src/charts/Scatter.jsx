// Named points with their Pareto frontier (lower x and lower y are better): the hedge efficiency frontier.
import { tick } from "../lib/format.js";
import { Axes, Frame, extent, scale, ticks } from "./core.jsx";

export function Frontier({ points, xLabel, yLabel, height = 220, caption }) {
  const pts = points.filter((p) => Number.isFinite(p.x) && Number.isFinite(p.y));
  if (!pts.length) return null;
  const sorted = [...pts].sort((a, b) => a.x - b.x);
  const front = [];
  let best = Infinity;
  for (const p of sorted) if (p.y <= best) { front.push(p); best = p.y; }
  const on = new Set(front);
  const [xlo, xhi] = extent(pts.map((p) => p.x), { pad: 0.12 });
  const [ylo, yhi] = extent(pts.map((p) => p.y), { pad: 0.15 });
  return (
    <Frame height={height} caption={caption}>
      {(w, h) => {
        const x0 = 52, x1 = w - 16, y0 = 14, y1 = h - 34;
        const xs = scale(xlo, xhi, x0, x1), ys = scale(ylo, yhi, y1, y0);
        return (
          <>
            <Axes x0={x0} x1={x1} y0={y0} y1={y1} xs={xs} ys={ys} xLabel={xLabel} yLabel={yLabel}
              xTicks={ticks(xlo, xhi, 4)} yTicks={ticks(ylo, yhi, 4)} />
            {front.length > 1 && <path class="s3 line" stroke-dasharray="4 3" d={front.map((p, i) => `${i ? "L" : "M"}${xs(p.x)},${ys(p.y)}`).join("")} />}
            {(() => {
              const placed = [];  // label boxes so far: a label that would overlap one goes below its point
              return pts.map((p) => {
                const x = xs(p.x), half = String(p.name).length * 2.9, above = ys(p.y) - 9;
                const hit = (y) => placed.some((b) => Math.abs(b.x - x) < b.half + half && Math.abs(b.y - y) < 11);
                const y = hit(above) ? ys(p.y) + 16 : above;
                placed.push({ x, y, half });
                return (
                  <g>
                    <circle class={`${on.has(p) ? "s3" : "muted"} dot`} cx={x} cy={ys(p.y)} r="5"><title>{`${p.name}: ${tick(p.x)}, ${tick(p.y)}`}</title></circle>
                    <text class={on.has(p) ? "value" : "tick"} x={x} y={y} text-anchor="middle">{p.name}</text>
                  </g>
                );
              });
            })()}
          </>
        );
      }}
    </Frame>
  );
}
