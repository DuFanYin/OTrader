"""Whether a backtest can be believed, on synthetic days whose answers are known.

- Golden: with no noise in the underlying, the model says what every option is quoted at every minute. Each fill of a
  backtest is at the costs' fill position in the model's spread (its mid plus that many half spreads toward its side:
  1 is the ask buying, the bid selling) at the bar it fills on, and each
  day's net P&L is what those fills and the fees make: to the cent.
- No look-ahead: change every quote after a minute of the day and run every demo strategy again (the combo as a single
  option, a delta-hedged straddle, an iron condor and entered on the Heston model's edge); what each did and logged up
  to that minute is the same."""

from __future__ import annotations

import math
import random
import re
from datetime import date, datetime, timezone

import polars as pl
import pytest

from optra.research import jobs, runner, strategies
from optra.data import synthetic_days
from optra.data.days import read_option_day
from optra.utils import files
from optra.utils.files import repo_root

pytestmark = pytest.mark.skipif(not (repo_root() / "engine" / "build" / "entry_backtest").exists(),
                                reason="entry_backtest is not built")

OPTION = re.compile(r"-(\d{8})-([CP])-([\d.]+)$")
POSITION = 0.6  # a fill position inside the spread: the rule, not only the quotes


def fee(price: float, contracts: float) -> float:
    """IBKR's commission on an ETF option, an order of one fill: by the price, at least 1 an order."""
    each = 0.25 if price < 0.05 else 0.50 if price < 0.10 else 0.65
    return max(contracts * each, 1.0)


def run(files_: list, strategy: str, setting: dict, log: bool = False) -> dict:
    trial = {"id": "t", "strategy": strategy, "setting": setting,
             "costs": {"fee_scale": 1.0, "fill_position": POSITION}, "fills": True, "log": log}
    return jobs.run({"files": [str(f) for f in files_], "risk_free_rate": 0.05, "iv_price_mode": "mid",
                     "trials": [trial]}, jobs.PRIORITY_REPORT)[1][0]


def spots(m: synthetic_days.Model, days: list[date]) -> dict[datetime, float]:
    """The underlying's mid at every minute, as the model makes it (no noise: the same whatever the draws)."""
    rng, out, open_px, last = random.Random(0), {}, m.spot, None
    for day in days:
        stamps = synthetic_days.session_minutes(day)
        if last is not None:
            open_px = synthetic_days.next_open(m, rng, last[1], last[0], stamps[0])
        path = synthetic_days.simulate_session(m, rng, open_px, len(stamps))
        for ts, px in zip(stamps, path):
            bid, ask = synthetic_days.underlying_quote(px, m)
            out[ts.astimezone(timezone.utc)] = (bid + ask) / 2
        last = (stamps[-1], path[-1])
    return out


def test_a_backtest_makes_what_the_model_says_to_the_cent(tmp_path, monkeypatch):
    m = synthetic_days.Model(realized_vol=0.0, drift=3.0, expiries=1, strikes=10)
    days = synthetic_days.trading_days(date(2023, 1, 2), date(2023, 1, 13))
    synthetic_days.generate(m, days[0], days[-1], tmp_path / "backtest", "SPY", False)
    monkeypatch.setattr(files, "data_root", lambda: tmp_path)
    mid = spots(m, days)
    setting = {"signal_minutes": 30, "entry_minute": 360, "exit_minute": 380}
    out = run(sorted((tmp_path / "backtest" / "SPY").rglob("*.parquet")), "SyntheticMomentumStrategy", setting)
    assert not out["errors"], out["errors"]

    def quoted(symbol: str, at: datetime) -> tuple[float, float]:
        expiry, right, strike = OPTION.search(symbol).groups()
        expires = synthetic_days.engine_expiry(datetime.strptime(expiry, "%Y%m%d").date())
        t = synthetic_days.years_to_expiry(at, expires)
        return synthetic_days.quote(synthetic_days.price(m, mid[at], float(strike), t, right == "C"), m)

    fills_of: dict[int, list] = {}
    for f in runner.rows(out["fills"]):
        fills_of.setdefault(f["date"], []).append(f)
    traded = 0
    for day in runner.rows(out["days"]):
        fills = fills_of.get(day["date"], [])
        assert [f["side"] for f in fills] == ["BUY", "SELL"]  # one call bought, sold before the close
        cash = 0.0
        for f, minute in zip(fills, (setting["entry_minute"] + 1, setting["exit_minute"] + 1)):
            at = datetime.strptime(f["time"], "%Y-%m-%d %H:%M:%S").replace(tzinfo=timezone.utc)
            session = synthetic_days.session_minutes(at.astimezone(synthetic_days.ET).date())
            assert at == session[minute].astimezone(timezone.utc)  # the bar after the order
            bid, ask = quoted(f["symbol"], at)
            buying = f["side"] == "BUY"
            expected = (ask + bid) / 2 + (1 if buying else -1) * POSITION * (ask - bid) / 2
            assert f["price"] == pytest.approx(expected, abs=1e-9), f
            if buying:  # as many as the premium budget buys at the ask it saw, the minute before
                seen = quoted(f["symbol"], session[minute - 1].astimezone(timezone.utc))[1]
                assert f["quantity"] == math.floor(1500 / (seen * 100))
                assert OPTION.search(f["symbol"]).group(2) == "C"  # the underlying rose: a call
            cash += (-1 if buying else 1) * f["price"] * f["quantity"] * 100 - fee(f["price"], f["quantity"])
        assert day["net_pnl"] == pytest.approx(cash, abs=0.01)
        traded += 1
    assert traded == len(days)
    assert max(out["days"]["max_margin"]) == 0  # options bought are paid for: no margin


def test_no_strategy_sees_past_the_bar_it_is_at(tmp_path, monkeypatch):
    m = synthetic_days.Model(realized_vol=0.25, expiries=2, strikes=10, seed=7)
    synthetic_days.generate(m, date(2023, 1, 2), date(2023, 1, 3), tmp_path / "a", "SPY", False)
    monkeypatch.setattr(files, "data_root", lambda: tmp_path)
    cut = datetime(2023, 1, 3, 13, 0, tzinfo=synthetic_days.ET)  # the second day, 13:00
    # The same days, every quote after the cut moved: up 3%, and the options' 30% dearer.
    for f in sorted((tmp_path / "a").rglob("*.parquet")):
        later = pl.col("ts_recv") > cut
        moved = read_option_day(f).with_columns(
            *(pl.when(later).then(pl.col(c) * 1.3).otherwise(pl.col(c)).alias(c) for c in ("bid_px", "ask_px")),
            *(pl.when(later).then(pl.col(c) * 1.03).otherwise(pl.col(c)).alias(c)
              for c in ("underlying_bid_px", "underlying_ask_px")))
        target = tmp_path / "b" / f.relative_to(tmp_path / "a")
        target.parent.mkdir(parents=True, exist_ok=True)
        moved.write_parquet(target)

    def before_cut(out: dict) -> list:
        """What the run did and said up to the cut: each day's fills and log, at their times (UTC)."""
        until = cut.astimezone(timezone.utc).strftime("%Y-%m-%d %H:%M:%S")
        said = runner.rows(out["fills"]) + runner.rows(out["log"])
        return [[(r["time"], r.get("symbol"), r.get("price"), r.get("quantity"), r.get("msg"))
                 for r in said if r["date"] == on and r["time"] <= until] for on in out["days"]["date"]]

    demo = {name for name, c in strategies.strategy_listing().items() if strategies.from_demo(c["source"])}
    straddle = {"leg1_ratio": 1, "leg1_dte": 1, "leg2_ratio": 1, "leg2_right": 1, "leg2_dte": 1, "entry_minute": 5}
    cases = [(name, {}) for name in sorted(demo)] + [
        ("ComboStrategy", {**straddle, "leg1_ratio": -1, "leg2_ratio": -1, "hold": 1, "hedge": 1}),
        ("ComboStrategy", {"leg1_ratio": -1, "leg1_right": 1, "leg1_strike_pct": -0.5, "leg2_ratio": 1,
                           "leg2_right": 1, "leg2_strike_pct": -1, "leg3_ratio": -1, "leg3_strike_pct": 0.5,
                           "leg4_ratio": 1, "leg4_strike_pct": 1}),
        ("ComboStrategy", {**straddle, "model": 1}),
    ]
    acted = 0
    for name, setting in cases:
        runs = [run(sorted((tmp_path / side).rglob("*.parquet")), name, setting, log=True) for side in ("a", "b")]
        seen = [before_cut(r) for r in runs]
        assert seen[0] == seen[1], (name, setting)
        acted += bool(runs[0]["fills"]["date"])
    assert acted >= 4  # not a test of strategies that do nothing
