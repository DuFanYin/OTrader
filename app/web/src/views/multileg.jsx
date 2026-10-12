// Multi-Leg: a strategy of several options, priced leg by leg (BS and MC) with its net Greeks.
import { post } from "../lib/api.js";
import { fmt } from "../lib/format.js";
import * as P from "../lib/payloads.js";
import { begin, fail, fill } from "../lib/runner.js";
import { params } from "../lib/store.js";
import { Kv, Slot, Table } from "../ui/blocks.jsx";
import { Group, Num } from "../ui/controls.jsx";
import { LegsInput, straddleAt } from "./legs.jsx";
import { MarketGroup, PrecheckGroup } from "./shared.jsx";

const ID = "multileg";

function Params() {
  return (
    <>
      <Group title="Legs">
        <LegsInput k="multiLegLegs" label="Strategy" title="Multi-leg strategy" empty="Empty: a straddle at the strike" />
        <Num k="nPaths" label="MC paths" step="1000" wide />
      </Group>
      <MarketGroup />
      <PrecheckGroup />
    </>
  );
}

async function run() {
  const p = params.value;
  begin(ID, ["strategy"]);
  let pos;
  try {
    pos = P.position(p.multiLegLegs) ?? { label: "Straddle at the strike", legs: straddleAt(p.strike) };
  } catch (err) {
    return fail(ID, "strategy", err.message);
  }
  await fill(ID, "strategy", post("api/lab/pricing/multi-leg", {
    spot: pos.spot ?? p.spot, rate: pos.rate ?? p.rate, vol: p.vol, maturity: p.maturity, dividend_yield: p.dividendYield, n_paths: p.nPaths, legs: pos.legs,
  }).then((d) => ({ ...d, label: pos.label })));
}

function Strategy({ data }) {
  const s = data.result_summary || {}, legs = data.result_details?.legs || [];
  return (
    <>
      <div class="flex flex-wrap items-baseline gap-x-4 gap-y-1">
        <span class="text-2xl font-semibold tracking-tight">{fmt(s.net_bs_price)}</span>
        <span class="text-sm text-muted">net (BS) · {data.label || s.strategy_hint || "custom"}</span>
      </div>
      <Kv rows={[["MC net", s.net_mc_price], ["Net Δ", s.net_delta], ["Net vega", s.net_vega], ["Strategy", s.strategy_hint || "custom"]]} />
      <Table compact rows={legs.map((l, i) => ({ ...l, n: i + 1, leg: `${l.quantity >= 0 ? "Long" : "Short"} ${(l.option_type || "call").toUpperCase()}` }))} cols={[
        { key: "n", label: "#", align: "left" }, { key: "leg", label: "Leg", align: "left" }, { key: "strike", label: "Strike" }, { key: "quantity", label: "Qty" },
        { key: "bs_price", label: "BS" }, { key: "mc_price", label: "MC" }, { key: "delta_bs", label: "Δ" }, { key: "vega_bs", label: "Vega" },
      ]} />
    </>
  );
}

const Results = () => <Slot mode={ID} slot="strategy" title="Multi-leg strategy" wide>{(d) => <Strategy data={d} />}</Slot>;

const about = "Price a strategy of several legs, from straddles and spreads to iron condors or your own, leg by leg with Black-Scholes and Monte Carlo, with the net Greeks. Edit the legs in the sidebar, or send a strategy here from the Screener.";

export default { id: ID, label: "Multi-Leg", about, Params, Results, run, precheck: true };
