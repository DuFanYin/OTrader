// State: the inputs of every view (one signal, saved in localStorage), the settings, and the results of the
// research views. Components read `params.value.x` and write with setParam("x", v).
import { signal, effect } from "@preact/signals";

const PARAMS_KEY = "optra_params_v1";
const SETTINGS_KEY = "optra_settings_v1";

export const DEFAULTS = {
  // backtest
  btSymbol: "", btStart: "", btEnd: "", btStrategy: "",
  btCosts: {},  // cost -> value where the value is not the default
  btRiskFreeRate: 0.05, btIvMode: "mid",
  btSettings: {},  // strategy class -> { parameter: value } where the value is not the default
  // experiments
  exFamily: "", exNote: "", exSymbol: "", exStart: "", exEnd: "", exStrategy: "",
  exSpace: {},  // strategy class -> { parameter: its space as typed ("15, 30", "10..60/10") }
  exSearch: "grid", exN: 50, exSeed: 0, exCosts: {},
  // tools
  toolsFilter: "", toolsOpen: "strategy_api",
  // live
  liveClass: "", livePortfolio: "", liveName: "", liveSettings: {},
  // account
  acValues: "main", acPositions: "open", acPortfolio: "", acExpiry: "", acPrice: null,
  acBookStrategy: "", acBookSymbol: "", acBookSide: "BUY", acBookQuantity: null, acBookPrice: null, acBookReason: "",
  // orders and trades
  otType: "all", otStrategy: "", otLimit: 200,
  // contracts from IB
  syncPortfolio: "", syncMaxDte: 7,
  // the option
  spot: 65000, strike: 65000, rate: 0.045, vol: 0.8, maturity: 0.25, dividendYield: 0, mu: 0,
  optionType: "call", exercise: "european", numeraire: "money_market", fxMode: false,
  nPaths: 20000, mcSampler: "pseudorandom",
  // pricing
  pricingMode: "single", batchJobs: 16, batchSpotShock: 0.02,
  ivSmilePoints: 17, ivKSteps: 15, ivTSteps: 11, burstLower: 10, burstUpper: 90, hestonMaxIter: 500,
  // multi-leg and risk
  multiLegLegs: "",  // empty: a straddle at the strike
  stressLegs: "", stressPack: "core4", stressSeverity: "moderate", stressIncludeHedge: true,
  nReb: 52, hedgePaths: 2000, hedgeTcBps: 5, hedgeThreshold: 0.02, hedgeVolMismatch: 1.15,
  // numerics
  pdeSSteps: 160, pdeTSteps: 160, pdeMethod: "crank_nicolson", measureMode: "pq",
  cmpSteps: 400, cmpPaths: 10000, measureN: 500, convSteps: "10,20,40,80,120,200,320,500", benchmarkBase: "pde",
  // validation
  pickStats: true, pickIto: true, pickSimulation: true, pickLattice: false, pickSmile: false, precheck: true, blockOnFail: false,
  statsTheta: 1.0, statsN: 50000, itoTheta: 0.7, itoT: 1.0, itoN: 5000, itoFunction: "exp_martingale",
  simSteps: 300, simDt: 0.01, simKappa: 1.2, simTheta: 0.03, simModel: "brownian",
  // screener
  scrView: "strategies", scrCurrency: "BTC", scrSnapshot: "", scrAt: "",
  scrSingle: false, scrIc: true, scrStraddle: false, scrStrangle: true, scrFwdVol: false, scrDirection: "LONG",
  scrMinOi: 1, scrMinVolume: null, scrMinPrice: null, scrMaxSpreadPct: 0.2, scrDaysLo: 1, scrDaysHi: 60,
  scrMoneyLo: null, scrMoneyHi: null, scrTwoSided: true,
  scrDebitLo: null, scrDebitHi: null, scrCreditLo: null, scrCreditHi: null, scrLossLo: null, scrLossHi: null,
  scrRrLo: null, scrRrHi: null, scrDeltaLo: null, scrDeltaHi: null, scrIvLo: null, scrIvHi: null,
  scrEdgeLo: null, scrEdgeHi: null, scrFwdVolLo: null, scrFwdVolHi: null,
  scrModelVol: "mark", scrHestonFit: true, scrHestonV0: 0.25, scrHestonKappa: 2.0, scrHestonTheta: 0.25, scrHestonXi: 0.8, scrHestonRho: -0.3,
  scrPriceMode: "executable", scrRankKey: "rr", scrTopN: 20,
};

function load(key, defaults) {
  try {
    const saved = JSON.parse(localStorage.getItem(key) || "{}");
    return { ...defaults, ...Object.fromEntries(Object.entries(saved).filter(([k]) => k in defaults)) };
  } catch {
    return { ...defaults };
  }
}
function persist(key, sig) {
  effect(() => {
    try { localStorage.setItem(key, JSON.stringify(sig.value)); } catch { /* private mode: nothing kept */ }
  });
}

export const params = signal(load(PARAMS_KEY, DEFAULTS));
persist(PARAMS_KEY, params);
export const setParam = (key, value) => { params.value = { ...params.value, [key]: value }; };
export const setParams = (values) => { params.value = { ...params.value, ...values }; };
export const resetParams = () => { params.value = { ...DEFAULTS }; };

export const settings = signal(load(SETTINGS_KEY, { precision: 4, dataMode: "live", tab: "home", labView: "screener" }));
persist(SETTINGS_KEY, settings);
export const setSetting = (key, value) => { settings.value = { ...settings.value, [key]: value }; };

// results of the research views: view id -> { slot name -> { state: "running" | "ok" | "error", data, error } }, plus the run's timing
export const results = signal({});
export const runs = signal({});  // view id -> { started, wall, running }
