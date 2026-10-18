// The views, in tab order: trading (the live engine: Home, the account, orders, data), then research (experiments,
// backtests, the Lab of calculators); the tools, a map to read rather than a place to work, apart on the right. A view:
// { id, label, about, enter, Params, Results, hasResults, status, action, research?, inner? }, where action is the one
// primary button: { label, busyLabel, busy (a signal), run, stop? }, and inner (the Lab) the view it stands for now.
import { accountView } from "./account.jsx";
import { backtest } from "./backtest.jsx";
import { data } from "./data.jsx";
import { experiments } from "./experiments.jsx";
import { home } from "./home.jsx";
import { calculators, lab } from "./lab.jsx";
import multileg from "./multileg.jsx";
import numerics from "./numerics.jsx";
import { orders } from "./orders.jsx";
import pricing from "./pricing.jsx";
import { researchView } from "./research.jsx";
import risk from "./risk.jsx";
import screener from "./screener.jsx";
import { tools } from "./tools.jsx";
import validation from "./validation.jsx";

const calculatorViews = [screener, pricing, multileg, risk, numerics, validation].map(researchView);
calculators.push(...calculatorViews);

export const GROUPS = [
  ["Trading", [home, accountView, orders, data]],
  ["Research", [experiments, backtest, lab]],
];
/** The tabs on the right, apart from the groups. */
export const APART = [tools];
/** Every view: the tabs, then the calculators inside the Lab. */
export const VIEWS = [...GROUPS.flatMap(([, views]) => views), ...APART, ...calculatorViews];
export const byId = Object.fromEntries(VIEWS.map((v) => [v.id, v]));
/** The tab a view is on: its own, or the Lab for a calculator. */
export const tabOf = (id) => (calculatorViews.some((v) => v.id === id) ? "lab" : id);
