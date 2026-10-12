"""The trading engine's tools: the backtest days and strategy classes there are, a report of strategies
over backtest days, a combo from the screener backtested as ComboStrategy, the hedge study, and the
live holdings under stress. Declared like every tool, so reached the same ways (HTTP and Python);
results come in the one envelope and, but for the lookups, are kept in the run
history (list_runs, get_run, compare_runs).

None of them places an order: a backtest's result carries the strategy as POST /api/strategies takes
it, and the operator adds it to the paper account.
"""

from __future__ import annotations

from datetime import date
from pathlib import Path
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field

from optra.tools.base import Result, Tool, ToolError

from optra.infra.backtest_runner import list_strategies_cpp
from optra.services import chains, combo, research, stress

FEE_RATE = Field(1.2, ge=0, description="Fees, USD an option contract (IBKR plus exchange fees on SPX: about 1.2); "
                                        "shares pay IBKR's fixed rate, 0.005 a share, 1 an order.")
SLIPPAGE = Field(None, ge=0, le=500, description="Market orders fill this many basis points worse than the quote "
                                                 "(entry_backtest's default: 5).")
HOLDOUT = Field(None, description="Days from this one on are the holdout, reported apart from the research days.")


class Input(BaseModel):
    model_config = ConfigDict(extra="forbid")


class DaysRequest(Input):
    symbol: str | None = Field(None, description="An underlying (SPXW, SPY); none: every one with days.")


class NoInput(Input):
    pass


class DayRange(Input):
    symbol: str = Field("SPXW", description="The underlying: SPXW or SPY (backtest_days lists what there is).")
    start: date | None = Field(None, description="The first day; none: the first there is.")
    end: date | None = Field(None, description="The last day; none: the last there is.")
    source: Literal["both", "backtest", "recorded"] = Field(
        "both", description="Days from historical data (backtest), days the live engine recorded (recorded), or both "
                            "(a recorded day wins).")


class ReportRequest(Input):
    days: str | None = Field(None, description="A folder of backtest days, or one day, relative to the repository; "
                                               "or leave it out and give symbol (with start, end, source).")
    symbol: str | None = Field(None, description="The underlying whose days to run (instead of days).")
    start: date | None = Field(None, description="With symbol: the first day.")
    end: date | None = Field(None, description="With symbol: the last day.")
    source: Literal["both", "backtest", "recorded"] = Field(
        "both", description="With symbol: days from historical data (backtest), recorded by the live engine "
                            "(recorded), or both (a recorded day wins).")
    strategies: list[str] = Field(description='Each a class (strategy_classes lists them), or '
                                              '"Class:name=value,name=value" with its parameters.')
    holdout_from: date | None = HOLDOUT
    fee_rate: float = FEE_RATE
    slippage_bps: float | None = SLIPPAGE
    risk: list[str] = Field(default_factory=list, description="Risk limits for the backtest, as risk.name=value.")


class ComboLeg(Input):
    option_type: Literal["call", "put"] = Field(description="The leg's right.")
    ratio: int = Field(description="Contracts per combo unit: bought +, sold - (the screener's qty).", ge=-10, le=10)
    strike_pct: float | None = Field(None, ge=-50, le=50, description=(
        "The strike as a percent from its expiry's forward (the nearest listed is taken). Give it with dte, or give "
        "strike, expiry and forward (a contract as the screener or Multi-Leg shows it) with as_of."))
    dte: int | None = Field(None, ge=0, le=365, description="Its expiry: the first at least this many days out.")
    strike: float | None = Field(None, gt=0, description="The contract's strike (with expiry, forward and as_of).")
    expiry: date | None = Field(None, description="The contract's expiry.")
    forward: float | None = Field(None, gt=0, description="The forward its expiry was priced on, on as_of.")


class ComboRequest(DayRange):
    legs: list[ComboLeg] = Field(min_length=1, max_length=4, description="One to four legs.")
    as_of: date | None = Field(None, description="The day legs given by strike and expiry were picked on.")
    entry_minute: int = Field(30, ge=1, le=389, description="Minutes after the 09:30 New York open it opens (30: 10:00).")
    exit_minute: int = Field(380, ge=2, le=390, description="Minutes after the open it closes (380: 15:50).")
    quantity: int = Field(1, ge=1, le=100, description="Combo units.")
    take_profit: float = Field(0, ge=0, le=100, description="Close once the P&L reaches this times the premium; 0: never.")
    stop_loss: float = Field(0, ge=0, le=100, description="Close once it loses this times the premium; 0: never.")
    model: Literal["none", "heston"] = Field("none", description=(
        "heston: open only when the combo's value under Heston, calibrated to each leg's chain at entry, less its cost "
        "at the executable prices is at least min_edge (the screener's edge with model_vol heston)."))
    min_edge: float = Field(0, description="USD a combo unit, with model heston.")
    holdout_from: date | None = HOLDOUT
    fee_rate: float = FEE_RATE
    slippage_bps: float | None = SLIPPAGE

    def setting(self, **hedge: float) -> dict[str, float]:
        return combo.setting([leg.model_dump(mode="json") for leg in self.legs], self.as_of,
                             entry_minute=self.entry_minute, exit_minute=self.exit_minute, quantity=self.quantity,
                             take_profit=self.take_profit, stop_loss=self.stop_loss,
                             model=1 if self.model == "heston" else 0, min_edge=self.min_edge, **hedge)

    def files(self) -> list[Path]:
        return combo.days(self.symbol, self.start, self.end, self.source)


class BacktestComboRequest(ComboRequest):
    hedge: bool = Field(False, description="The HedgeEngine keeps its delta within delta_range shares of zero.")
    delta_range: int = Field(20, ge=0, le=1000, description="Shares either side of zero left alone.")
    hedge_timer: int = Field(1, ge=1, le=390, description="Hedge every this many minutes.")


class HedgeStudyRequest(ComboRequest):
    delta_ranges: list[int] = Field([5, 20, 50], min_length=1, max_length=8,
                                    description="The bands to compare, shares either side of zero; unhedged is always run.")
    hedge_timer: int = Field(1, ge=1, le=390, description="Hedge every this many minutes.")


class StressRequest(Input):
    pack: str = Field("core4", description="The stress pack (stress_packs lists them).")
    severity: Literal["mild", "moderate", "severe"] = Field(
        "moderate", description="How hard the pack's shocks hit: mild 0.7x, moderate 1x, severe 1.5x.")


def _bad(exc: Exception) -> ToolError:
    return ToolError(str(exc), 400)


def backtest_days(p: DaysRequest) -> Result:
    names = [p.symbol] if p.symbol else chains.symbols()
    out = {}
    for name in names:
        found = chains._day_files(name)
        out[name] = {"days": len(found), "first": min(found, default=None), "last": max(found, default=None),
                     "recorded": sorted(d for d, (_, kind) in found.items() if kind == "recorded")}
    return Result(summary={"symbols": out})


def strategy_classes(_: NoInput) -> Result:
    listed = list_strategies_cpp()
    if listed.get("status") != "ok":
        raise ToolError(listed.get("error", "entry_backtest --list-strategies failed"), 503)
    return Result(summary={"strategies": listed["strategies"]},
                  notes=["Run one with backtest_report: \"Class:name=value,...\"; add one to the paper account with "
                         "POST /api/strategies {strategy_class, portfolio_name, setting}."])


def backtest_report(p: ReportRequest) -> Result:
    try:
        if p.days:
            days: Path | list[Path] = Path(p.days)
        elif p.symbol:
            days = combo.days(p.symbol, p.start, p.end, p.source)
        else:
            raise ValueError("give days (a folder) or symbol")
        out = research.report(days, p.strategies, p.holdout_from, p.fee_rate, p.risk, p.slippage_bps)
    except ValueError as exc:
        raise _bad(exc) from exc
    return Result(summary={"days": out["days"], "strategies": len(p.strategies), "header": out["header"]},
                  details={"periods": out["periods"], "trades": out["trades"], "markdown": out["markdown"],
                           "errors": out["errors"]},
                  warnings=[f"{s}: {e}" for s, errs in out["errors"].items() for e in errs])


def _report_summary(out: dict[str, Any]) -> dict[str, Any]:
    rows = out["periods"][-1]["rows"]  # all days
    row = rows[0] if rows else {}
    return {"days": out["days"], **{k: row.get(k) for k in (
        "trades", "win rate", "expectancy R", "t", "net USD", "drawdown USD", "fees", "attribution")}}


def backtest_combo(p: BacktestComboRequest) -> Result:
    try:
        values = p.setting(hedge=int(p.hedge), delta_range=p.delta_range, hedge_timer=p.hedge_timer)
        out = combo.backtest(p.symbol, values, p.files(), p.holdout_from, p.fee_rate, [], p.slippage_bps)
    except ValueError as exc:
        raise _bad(exc) from exc
    return Result(summary=_report_summary(out),
                  details={"strategy": out["strategy"], "periods": out["periods"],
                           "trades": next(iter(out["trades"].values()), []), "header": out["header"]},
                  notes=["attribution: the P&L before fees by source (the engine's: delta, gamma, theta, vega, other, "
                         "fills against the mid); fees apart.",
                         "details.strategy is what POST /api/strategies takes to run the same on the paper account."],
                  warnings=[e for errs in out["errors"].values() for e in errs])


def hedge_study(p: HedgeStudyRequest) -> Result:
    try:
        out = combo.hedge_study(p.setting(), p.files(), p.delta_ranges, p.hedge_timer, p.fee_rate, [],
                                p.slippage_bps)
    except ValueError as exc:
        raise _bad(exc) from exc
    return Result(summary={"least_tail_loss": out["least_tail_loss"],
                           "variants": [{k: r.get(k) for k in ("variant", "days", "mean", "std", "es95", "hedge_fills",
                                                               "fees")} for r in out["rows"]]},
                  details={"rows": out["rows"]},
                  notes=["Per variant, the daily P&L after fees: the hedging tool's measures (mean, std, q05/q50/q95, "
                         "var95, es95 = the mean loss of the worst 5% of days), on days that happened."],
                  warnings=[f"{v}: {e}" for v, errs in out["errors"].items() for e in errs])


def holdings_stress(p: StressRequest) -> Result:
    from optra.services.main import MainService  # the live engine's client, at call time

    try:
        holdings = MainService._get_client_or_400().get_strategy_holdings()["holdings"]
    except Exception as exc:  # no engine: say so as the tool's error
        raise ToolError(f"the live engine is not reachable: {getattr(exc, 'detail', exc)}", 503) from exc
    try:
        out = stress.stress(holdings, p.pack, p.severity)
    except ValueError as exc:
        raise _bad(exc) from exc
    worst = out["worst"] or {}
    return Result(summary={"worst_scenario": worst.get("scenario"), "worst_pnl": worst.get("pnl"),
                           "legs": out["legs"], "marked_at": out["marked_at"]},
                  details={k: out[k] for k in ("scenarios", "strategies", "total", "unpriced", "marked_on")},
                  warnings=out["warnings"])


ENGINE = "The trading engine's. "
COMBO_EXAMPLE = {"symbol": "SPXW", "legs": [{"option_type": "call", "ratio": 1, "strike_pct": 0, "dte": 0},
                                            {"option_type": "put", "ratio": 1, "strike_pct": 0, "dte": 0}]}

TOOLS = [
    Tool("backtest_days", "Backtest days",
         ENGINE + "The days there are to backtest on, per underlying: how many, the first and last, and which the live "
         "engine recorded.", DaysRequest, backtest_days, "/backtest/days", method="GET", engine=False,
         record=False, example={"symbol": "SPXW"}),
    Tool("strategy_classes", "Strategy classes",
         ENGINE + "The strategy classes the engine has, each with its parameters (default, min, max, integer).",
         NoInput, strategy_classes, "/backtest/strategies", method="GET", engine=False, record=False),
    Tool("backtest_report", "Backtest report",
         ENGINE + "Strategies over backtest days, a trade a day: trades, win rate, average win and loss in R (R = the "
         "day's P&L after fees / what the position opened could lose at most), payoff, expectancy with its t, net USD, "
         "losing streak, drawdown, the gap to RandomBuyStrategy, the P&L by source (delta, gamma, theta, vega, other, "
         "fills); the research days and the holdout apart. Returns result_details.periods (a row per strategy), "
         "trades (each day) and markdown.",
         ReportRequest, backtest_report, "/backtest/report", cost="heavy", engine=False,
         example={"symbol": "SPXW", "strategies": ["RandomBuyStrategy", "LateDayMomentumStrategy"]}),
    Tool("backtest_combo", "Backtest a combo",
         ENGINE + "A combo of one to four legs (from the screener or Multi-Leg, or rules: call or put, ratio, strike as "
         "a percent from the forward, days to expiry) opened each day at entry_minute and closed at exit_minute or "
         "on take_profit / stop_loss, run as the engine's ComboStrategy over the days; optionally hedged, and "
         "optionally only on a Heston edge. Returns the report's measures and P&L by source, each day, and "
         "result_details.strategy: the strategy as POST /api/strategies takes it, to run the same on paper.",
         BacktestComboRequest, backtest_combo, "/backtest/combo", cost="heavy", engine=False,
         example=COMBO_EXAMPLE),
    Tool("hedge_study", "Hedge study on backtest days",
         ENGINE + "The same combo over the same days unhedged and with its delta kept within each band: per variant "
         "the daily P&L after fees (mean, std, quantiles, var95, es95), the share fills, the fees and the P&L by "
         "source. The hedging tool's comparison, on days that happened instead of simulated paths.",
         HedgeStudyRequest, hedge_study, "/backtest/hedge-study", cost="heavy", engine=False,
         example={**COMBO_EXAMPLE, "delta_ranges": [5, 20, 50]}),
    Tool("holdings_stress", "Live holdings under stress",
         ENGINE + "Every live strategy's holding revalued under a stress pack and a spot ladder (+-1, 2, 5%), each "
         "option at its own implied vol and its expiry's forward from the newest quotes: P&L by strategy and in "
         "total, and the legs it could not value.",
         StressRequest, holdings_stress, "/risk/stress", method="GET", engine=True,
         example={"pack": "core4", "severity": "moderate"}, live=True),
]

