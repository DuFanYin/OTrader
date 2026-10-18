"""The engine's days as the lab's option chains: a synthetic day priced at a known implied vol reads
back that vol, calls and puts alike, at the time asked for."""

from __future__ import annotations

from datetime import date

import pytest

from optra.utils import files
from optra.services import market_data  # noqa: E402
from optra.infra.numerics import is_engine_available  # noqa: E402

from optra.data import synthetic_days  # noqa: E402
from optra.data import chains  # noqa: E402

pytestmark = pytest.mark.skipif(not is_engine_available(), reason="libanalytics_c.so not built")


@pytest.fixture()
def day(tmp_path, monkeypatch):
    model = synthetic_days.Model(implied_vol=0.2, tick=0.0, spread=0.0)
    synthetic_days.generate(model, date(2025, 3, 3), date(2025, 3, 3), tmp_path, "SPY", False)
    monkeypatch.setattr(files, "data_root", lambda: tmp_path)
    return model


def test_a_day_reads_back_its_implied_vol(day):
    chain = chains.load("SPY", "2025-03-03", "10:30")
    assert chain["source"] == "backtest"
    assert chain["fetched_at"] == "2025-03-03T10:30:00-05:00"
    assert chain["multiplier"] == 100.0
    near = [r for r in chain["chain"] if abs(r["strike"] / r["forward"] - 1) < 0.02]
    assert {r["option_type"] for r in near} == {"call", "put"}
    for r in near:
        assert r["iv"] == pytest.approx(0.2, abs=2e-3), r["symbol"]


def test_the_engine_days_and_times(day):
    assert chains.symbols() == ["SPY"]
    assert "SPY" in market_data.chain_underlyings()
    assert market_data.list_chain_snapshots("SPY")[0]["snapshot_id"] == "SPY_20250303"
    early = market_data.load_chain_snapshot("SPY_20250303", "09:00")  # before the open: the first quotes
    assert early["fetched_at"] == "2025-03-03T09:30:00-05:00"
    last = market_data.load_chain_snapshot("SPY_20250303")
    assert last["fetched_at"] == "2025-03-03T16:00:00-05:00"
    with pytest.raises(ValueError):
        chains.load("SPY", "2025-03-04", None)
