// Parameter groups shared by several modes: the market inputs, the option, the validation pre-check.
import { fetchLive, market, markStrikeEdited, reresolve } from "../lib/market.js";
import { params, setSetting, settings } from "../lib/store.js";
import { Chips, Group, Num, Seg, Segmented, SmallButton, Toggle } from "../ui/controls.jsx";
import { DialogGroup } from "../ui/dialog.jsx";

const Info = ({ children }) => <p class="col-span-full -mt-1 truncate font-mono text-2xs text-muted">{children}</p>;

const DOT = { loading: "bg-warn animate-pulse", ok: "bg-up", error: "bg-down" };

function chooseSource(v) {
  setSetting("dataMode", v);
  if (v === "live") fetchLive({ force: true }).catch(() => {});
  else market.value = { state: "idle" };
}

/** The market inputs: live (BTC from Deribit and Binance, r from the Treasury curve) or simulated (edit them freely). */
export function MarketGroup({ drift }) {
  const live = settings.value.dataMode === "live";
  const m = market.value;
  const status = live && (
    <span class="flex items-center gap-1.5 text-2xs text-muted" title={m.error ?? m.state}>
      <span class={`h-1.5 w-1.5 rounded-full ${DOT[m.state] ?? "bg-line-strong"}`} />
      {m.at ? m.at.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" }) : m.state}
      <SmallButton title="Refresh market data" onClick={() => fetchLive({ force: true }).catch(() => {})}>↻</SmallButton>
    </span>
  );
  return (
    <Group title="Market" action={status}>
      <div class="col-span-full"><Segmented size="xs" value={settings.value.dataMode} options={[["live", "Live"], ["sim", "Simulated"]]} onChange={chooseSource} /></div>
      <Num k="spot" label="Spot" hint="BTC/USD index (Binance)" readOnly={live} decimals={live ? 2 : undefined} />
      <Num k="strike" label="Strike" onChange={() => { markStrikeEdited(); if (live) reresolve(); }} />
      <Num k="vol" label="σ" hint="Implied vol matched to strike and T (Deribit surface)" step="0.01" readOnly={live} decimals={live ? 4 : undefined} />
      <Num k="rate" label="r" hint="US Treasury yield interpolated to T" step="0.0001" readOnly={live} decimals={live ? 4 : undefined} />
      {live && m.fallback?.length > 0 && <Info>Not fetched, fixed values used: {m.fallback.join(", ")}</Info>}
      {live && m.ivSource && <Info>σ {m.ivSource} · {m.ivMatch}</Info>}
      {live && m.rateTenor && <Info>r tenor {m.rateTenor}</Info>}
      <Num k="maturity" label="T (years)" step="0.05" onChange={() => live && reresolve()} />
      <Num k="dividendYield" label="q" hint="BTC pays no dividend" step="0.01" />
      {drift && <Num k="mu" label="μ (drift)" hint="Annualised BTC perpetual funding rate (Binance)" step="0.001" readOnly={live} decimals={live ? 4 : undefined} wide />}
      {drift && live && m.funding8h != null && <Info>8h funding {m.funding8h}% → {m.muPct}% a year</Info>}
    </Group>
  );
}

export const OPTION_TYPES = [["call", "Call"], ["put", "Put"]];
export const EXERCISE = [["european", "European"], ["american", "American"]];

const SAMPLERS = [["pseudorandom", "Pseudo"], ["antithetic", "Antithetic"], ["sobol", "Sobol"]];
const NUMERAIRES = [["money_market", "Money market"], ["stock", "Stock"]];
const labelOf = (options, v) => options.find(([o]) => o === v)?.[1] ?? v;

export function OptionGroup({ mc = true }) {
  const p = params.value;
  return (
    <>
      <Group title="Option">
        <Seg k="optionType" label="Type" options={OPTION_TYPES} wide={false} />
        <Seg k="exercise" label="Exercise" options={EXERCISE} wide={false} />
      </Group>
      {mc && (
        <DialogGroup title="Monte Carlo" summary={`${Number(p.nPaths).toLocaleString()} paths · ${labelOf(SAMPLERS, p.mcSampler)} · `
          + `${labelOf(NUMERAIRES, p.numeraire).toLowerCase()} numeraire${p.fxMode ? " · FX" : ""}`}>
          <Num k="nPaths" label="Paths" step="1000" />
          <Seg k="mcSampler" label="Sampler" options={SAMPLERS} wide={false} />
          <Seg k="numeraire" label="Numeraire" options={NUMERAIRES} />
          <Toggle k="fxMode" label="FX mode" />
        </DialogGroup>
      )}
    </>
  );
}

export const CHECKS = [["pickStats", "Stats"], ["pickIto", "Itô"], ["pickSimulation", "Simulation"],
  ["pickLattice", "Lattice", "Trinomial vs BS; LSM and PSOR finite differences vs the binomial American put"]];

export function PrecheckGroup() {
  const p = params.value, picked = CHECKS.filter(([k]) => p[k]).map(([, label]) => label);
  return (
    <DialogGroup title="Pre-check" summary={p.precheck
      ? `${picked.join(", ") || "no checks"}${p.blockOnFail ? " · blocks on fail" : ""}` : "off"}>
      <Toggle k="precheck" label="Validate first" hint="Run the validation gate before the computation" />
      <Toggle k="blockOnFail" label="Block on fail" hint="Stop when the gate fails" />
      <Chips label="Checks (parameters under Validation)" items={CHECKS} />
    </DialogGroup>
  );
}

