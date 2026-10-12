// The views, in tab order: trading (the live engine: Home, the account, orders, data), then research (backtests,
// the numerics and the market). A view:
// { id, label, about, enter, Params, Results, hasResults, status, action, research? }, where action is the one
// primary button: { label, busyLabel, busy (a signal), run, stop? }.
import { accountView } from "./account.jsx";
import { backtest } from "./backtest.jsx";
import { data } from "./data.jsx";
import { home } from "./home.jsx";
import multileg from "./multileg.jsx";
import numerics from "./numerics.jsx";
import { orders } from "./orders.jsx";
import pricing from "./pricing.jsx";
import { researchView } from "./research.jsx";
import risk from "./risk.jsx";
import screener from "./screener.jsx";
import validation from "./validation.jsx";

export const GROUPS = [
  ["Trading", [home, accountView, orders, data]],
  ["Research", [backtest, ...[screener, pricing, multileg, risk, numerics, validation].map(researchView)]],
];
export const VIEWS = GROUPS.flatMap(([, views]) => views);
export const byId = Object.fromEntries(VIEWS.map((v) => [v.id, v]));
