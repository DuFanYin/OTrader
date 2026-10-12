// Request bodies, built from the parameters (store.js). One place for every field the API expects.
const LADDER = [10, 20, 40, 80, 120, 200, 320, 500];

export const base = (p) => ({
  spot: p.spot, strike: p.strike, rate: p.rate, vol: p.vol, maturity: p.maturity, n_paths: p.nPaths,
  option_type: p.optionType, is_american: p.exercise === "american", dividend_yield: p.dividendYield,
  numeraire: p.numeraire, fx_mode: p.fxMode, mc_sampler: p.mcSampler,
});

export function ladder(p) {
  const steps = String(p.convSteps ?? "").split(",").map((x) => Number(x.trim())).filter((x) => Number.isFinite(x) && x > 1);
  return steps.length ? steps : LADDER;
}

export const anyCheck = (p) => p.pickStats || p.pickIto || p.pickSimulation || p.pickLattice;

export const validationGate = (p) => ({
  pick_stats: p.pickStats, pick_ito: p.pickIto, pick_simulation: p.pickSimulation, pick_lattice: p.pickLattice,
  compute_block_on_validation: p.blockOnFail,
  spot: p.spot, strike: p.strike, rate: p.rate, vol: p.vol, maturity: p.maturity, n_paths: p.nPaths,
  dividend_yield: p.dividendYield, mu: p.mu,
  stats_theta: p.statsTheta, stats_n: p.statsN,
  ito_function_type: p.itoFunction, ito_theta: p.itoTheta, ito_t: p.itoT, ito_n: p.itoN,
  model: p.simModel, sim_steps: p.simSteps, sim_dt: p.simDt, sim_kappa: p.simKappa, sim_theta: p.simTheta,
  measure_n: p.measureN, cmp_steps: p.cmpSteps, cmp_paths: p.cmpPaths, conv_steps: ladder(p),
  pde_s_steps: p.pdeSSteps, pde_t_steps: p.pdeTSteps, pde_method: p.pdeMethod,
  n_rebalances: p.nReb, hedge_paths: p.hedgePaths, batch_jobs: p.batchJobs, batch_spot_shock: p.batchSpotShock,
});

export const greekSurface = (p, greek) => ({
  strike: p.strike, rate: p.rate, vol: p.vol, dividend_yield: p.dividendYield,
  spot_min: p.spot * 0.6, spot_max: p.spot * 1.4, mat_min: 0.05, mat_max: Math.max(0.1, p.maturity * 2),
  n_spots: 21, n_mats: 11, greek,
});

export const ivDiagnostics = (p) => ({
  spot: p.spot, strike: p.strike, maturity: p.maturity,
  smile_points: p.ivSmilePoints, moneyness_steps: p.ivKSteps, tenor_steps: p.ivTSteps,
  burst_lower_quantile: p.burstLower / 100, burst_upper_quantile: p.burstUpper / 100,
});

export const bsImpliedVol = (p) => ({
  market_price: p.spot * 0.1, spot: p.spot, strike: p.strike, rate: p.rate, maturity: p.maturity, dividend_yield: p.dividendYield,
});

/** Heston calibration to a synthetic smile around the strike (7 strikes × 2 maturities from a skewed BS vol). */
export function heston(p) {
  const S = p.spot, K = p.strike, r = p.rate, v = p.vol, T = p.maturity;
  const strikes = [], maturities = [], prices = [];
  const cdf = (x) => 0.5 * (1 + Math.sign(x) * Math.sqrt(1 - Math.exp((-2 * x * x) / Math.PI)));
  for (const km of [0.85, 0.9, 0.95, 1, 1.05, 1.1, 1.15]) for (const tm of [0.5, 1]) {
    const k = K * km, t = T * tm, m = Math.log(k / S) / Math.sqrt(t);
    const sv = Math.max(0.05, v * (1 + 0.15 * m * m - 0.1 * m));
    const d1 = (Math.log(S / k) + (r + 0.5 * sv * sv) * t) / (sv * Math.sqrt(t)), d2 = d1 - sv * Math.sqrt(t);
    strikes.push(k); maturities.push(t);
    prices.push(Math.max(S * cdf(d1) - k * Math.exp(-r * t) * cdf(d2), 0.001));
  }
  return { spot: S, rate: r, dividend_yield: p.dividendYield, market_strikes: strikes, market_maturities: maturities,
    market_prices: prices, init_v0: v * v, init_kappa: 1.5, init_theta: v * v, init_xi: 0.5, init_rho: -0.7, max_iter: p.hestonMaxIter };
}

export const hedge = (p) => ({
  n_rebalances: p.nReb, transaction_cost_bps: p.hedgeTcBps, rebalance_threshold: p.hedgeThreshold, vol_mismatch_mult: p.hedgeVolMismatch,
});

/** A position text box: a legs array, or {spot?, rate?, label?, legs}. null when empty; throws when malformed. */
export function position(text) {
  const raw = String(text ?? "").trim();
  if (!raw) return null;
  let parsed;
  try { parsed = JSON.parse(raw); } catch (err) { throw new Error(`Legs JSON is invalid: ${err.message}`); }
  const pos = Array.isArray(parsed) ? { legs: parsed } : parsed;
  if (!Array.isArray(pos?.legs) || !pos.legs.length) throw new Error('Legs JSON must be a non-empty array, or an object with a non-empty "legs" array');
  return pos;
}

/** A screener strategy as a multi-leg position (per-leg vol, maturity and forward). */
export function strategyPosition(strategy, summary) {
  const mult = Number(summary?.multiplier ?? 1);
  return {
    label: strategy.label, spot: summary?.spot, rate: summary?.rate,
    legs: (strategy.legs || []).map((l) => ({
      option_type: l.option_type, strike: l.strike, quantity: l.qty * mult, maturity: Number(l.years.toFixed(8)), forward: l.forward,
      ...(l.iv > 0 ? { vol: l.iv } : {}),
    })),
  };
}

const range = (lo, hi) => (lo == null && hi == null ? null : [lo, hi]);

export function screener(p) {
  const body = {
    currency: p.scrCurrency, snapshot_id: p.scrSnapshot || null, at: (p.scrSnapshot && p.scrAt) || null,
    strategies: { single_calls: p.scrSingle, iron_condors: p.scrIc, straddles: p.scrStraddle, strangles: p.scrStrangle, forward_vols: p.scrFwdVol },
    option_filter: {
      min_oi: p.scrMinOi, min_volume: p.scrMinVolume, min_price: p.scrMinPrice, max_bid_ask_spread_pct: p.scrMaxSpreadPct,
      days_to_expiry_range: range(p.scrDaysLo, p.scrDaysHi), moneyness_range: range(p.scrMoneyLo, p.scrMoneyHi), require_two_sided: p.scrTwoSided,
    },
    strategy_filter: {
      direction: p.scrDirection, debit_range: range(p.scrDebitLo, p.scrDebitHi), credit_range: range(p.scrCreditLo, p.scrCreditHi),
      potential_loss_range: range(p.scrLossLo, p.scrLossHi), rr_range: range(p.scrRrLo, p.scrRrHi), net_delta_range: range(p.scrDeltaLo, p.scrDeltaHi),
      iv_range: range(p.scrIvLo, p.scrIvHi), edge_range: range(p.scrEdgeLo, p.scrEdgeHi), forward_vol_range: range(p.scrFwdVolLo, p.scrFwdVolHi),
    },
    price_mode: p.scrPriceMode, model_vol: p.scrModelVol, rank: { key: p.scrRankKey, top_n: p.scrTopN ?? 20 },
  };
  if (p.scrModelVol === "heston" && !p.scrHestonFit) {
    body.heston = { v0: p.scrHestonV0, kappa: p.scrHestonKappa, theta: p.scrHestonTheta, xi: p.scrHestonXi, rho: p.scrHestonRho };
  }
  return body;
}
