// Vertical columns over an index (daily PnL): signed values go up / down from 0, gains in the gain colour.
import { tick } from "../lib/format.js";
import { Axes, Frame, extent, scale, ticks } from "./core.jsx";

export function Columns({ values, height = 120, caption, labels, fmt = tick }) {
  const v = values.map(Number);
  if (!v.length) return null;
  const [lo, hi] = extent(v, { zero: true, pad: 0.1 });
  return (
    <Frame height={height} caption={caption}>
      {(w, h) => {
        const x0 = 46, x1 = w - 8, y0 = 8, y1 = h - 18;
        const ys = scale(lo, hi, y1, y0), bw = (x1 - x0) / v.length;
        const every = Math.max(1, Math.ceil(v.length / Math.max(2, Math.floor((x1 - x0) / 72))));
        return (
          <>
            <Axes x0={x0} x1={x1} y0={y0} y1={y1} xs={(i) => x0 + (i + 0.5) * bw} ys={ys} yTicks={ticks(lo, hi, 3)}
              xTicks={v.map((_, i) => i).filter((i) => i % every === 0).map((i) => ({ x: i, label: labels?.[i] ?? i + 1 }))} />
            <line class="axis" x1={x0} x2={x1} y1={ys(0)} y2={ys(0)} />
            {v.map((x, i) => (
              <rect class={x >= 0 ? "up" : "down"} x={x0 + i * bw + bw * 0.2} width={Math.max(1, bw * 0.6)} rx="1.5" opacity="0.85"
                y={Math.min(ys(x), ys(0))} height={Math.max(1, Math.abs(ys(x) - ys(0)))}>
                <title>{`${labels?.[i] ?? i + 1}: ${fmt(x)}`}</title>
              </rect>
            ))}
          </>
        );
      }}
    </Frame>
  );
}
