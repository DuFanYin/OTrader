"""The holdings under stress scenarios: a known book on a synthetic day priced at a known vol."""

from __future__ import annotations

from datetime import date

import pytest

from optra.utils import files
from optra.infra.numerics import is_engine_available  # noqa: E402

from optra.data import synthetic_days  # noqa: E402
from optra.services import stress  # noqa: E402
from optra.data import chains, days

pytestmark = pytest.mark.skipif(not is_engine_available(), reason="libanalytics_c.so not built")


@pytest.fixture()
def chain(tmp_path, monkeypatch):
    model = synthetic_days.Model(implied_vol=0.2, tick=0.0, spread=0.0)
    synthetic_days.generate(model, date(2025, 3, 3), date(2025, 3, 3), tmp_path / "backtest", "SPY", False)
    monkeypatch.setattr(files, "data_root", lambda: tmp_path)
    return chains.load("SPY", "2025-03-03", "10:30")


def _atm(chain, expiry_index=2):
    expiry = chain["expiries"][expiry_index]["expiry"]
    rows = [r for r in chain["chain"] if r["expiry"] == expiry]
    return min(rows, key=lambda r: abs(r["strike"] - r["forward"]))


def _sym(row, right):
    return f"SPY-{row['expiry'].replace('-', '')}-{right}-{row['strike']:g}"


def test_a_book_under_stress(chain):
    atm = _atm(chain)
    holdings = {
        "Long": {"underlying": {"quantity": 0}, "options": {
            _sym(atm, "C"): {"symbol": _sym(atm, "C"), "quantity": 2, "multiplier": 100.0}}},
        "Straddle": {"underlying": {"quantity": -50, "mid_price": 600.0, "multiplier": 1.0}, "options": {
            _sym(atm, k): {"symbol": _sym(atm, k), "quantity": -1, "multiplier": 100.0, "combo_type": "straddle",
                           "combo": f"{_sym(atm, 'C')}|{_sym(atm, 'P')}"} for k in ("C", "P")}},
        "Junk": {"underlying": {"quantity": 0}, "options": {
            "SPY-20250303-C-1": {"symbol": "SPY-20250303-C-1", "quantity": 1, "multiplier": 100.0}}},
    }
    r = stress.stress(holdings, chain_of=lambda root: chain)
    assert r["legs"] == 3 and len(r["unpriced"]) == 1
    by = {s["strategy"]: s for s in r["strategies"]}
    call = next(x for x in chain["chain"] if x["symbol"].endswith(_sym(atm, "C")[-12:]) or
                (x["expiry"] == atm["expiry"] and x["strike"] == atm["strike"] and x["option_type"] == "call"))
    assert by["Long"]["value"] == pytest.approx(2 * 100 * call["mark"], rel=1e-3)
    up, down = by["Long"]["pnl"]["Spot +1%"], by["Long"]["pnl"]["Spot -1%"]
    assert up > 0 > down
    assert by["Long"]["pnl"]["Vol Spike"] > 0
    # short straddle: loses on any move and on a vol spike; the short shares gain when spot falls
    assert by["Straddle"]["pnl"]["Vol Spike"] < 0
    assert by["Straddle"]["pnl"]["Spot +5%"] < 0
    shares_part = -50 * 600.0 * -0.05
    assert by["Straddle"]["pnl"]["Spot -5%"] > by["Straddle"]["pnl"]["Spot +5%"] - 2 * shares_part
    assert r["total"]["Spot +1%"] == pytest.approx(sum(s["pnl"]["Spot +1%"] for s in r["strategies"]))
    assert r["worst"]["pnl"] == min(r["total"].values())


def test_parse_option():
    assert days.parse_option("SPXW-20261016-C-5800") == ("SPXW", date(2026, 10, 16), "C", 5800.0)
    assert days.parse_option("SPXW-20261016-C-582.5").strike == 582.5
    assert days.parse_option("SPY") is None
