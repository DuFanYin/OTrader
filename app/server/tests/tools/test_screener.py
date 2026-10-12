"""Screener engine tests on synthetic chains (sf_run_screener_json via numerics).

Run:  python tests/test_screener.py   (or pytest)
Requires a built engine: ./run.sh build
"""

from __future__ import annotations

import math
import sys

from optra.infra import numerics  # noqa: E402
from optra.version import CONTRACT_VERSION  # noqa: E402


def opt(kind, strike, *, expiry="E1", years=0.25, forward=100.0, bid=1.0, ask=1.2,
        iv=0.6, oi=100.0, volume=10.0, mark=None):
    return {
        "symbol": f"X-{expiry}-{strike:g}-{kind[0].upper()}", "option_type": kind, "expiry": expiry,
        "strike": float(strike), "forward": forward, "years": years, "bid": bid, "ask": ask,
        "mark": (bid + ask) / 2 if mark is None and bid is not None and ask is not None else (mark or 0.0),
        "iv": iv, "volume": volume, "oi": oi,
    }


def screen(chain, strategies, *, spot=100.0, rate=0.0, top_n=50, rank="cost", descending=None,
           option_filter=None, strategy_filter=None, **extra):
    rank_spec = {"key": rank, "top_n": top_n}
    if descending is not None:
        rank_spec["descending"] = descending
    r = numerics.screener({
        "spot": spot, "rate": rate, "chain": chain,
        "strategies": {k: True for k in strategies},
        "option_filter": option_filter or {},
        "strategy_filter": strategy_filter or {},
        "rank": rank_spec, **extra,
    })
    return r


def ok(r):
    assert r["status"] == "ok", r.get("error")
    return r["result_summary"], r["result_details"]["strategies"]


def black76(F, K, r, vol, T, is_call):
    n = lambda x: 0.5 * (1.0 + math.erf(x / math.sqrt(2.0)))  # noqa: E731
    sd = vol * math.sqrt(T)
    d1 = (math.log(F / K) + 0.5 * sd * sd) / sd
    d2 = d1 - sd
    df = math.exp(-r * T)
    return df * (F * n(d1) - K * n(d2)) if is_call else df * (K * n(-d2) - F * n(-d1))


# ── Pricing / greeks ──────────────────────────────────────────────────────────

def test_model_price_and_greeks_are_black76():
    F, K, r, vol, T = 105.0, 100.0, 0.03, 0.55, 0.4
    chain = [opt("call", K, forward=F, years=T, iv=vol), opt("put", K, forward=F, years=T, iv=vol)]
    _, rows = ok(screen(chain, ["straddles"], rate=r))
    call_leg, put_leg = rows[0]["legs"]
    assert abs(call_leg["model_price"] - black76(F, K, r, vol, T, True)) < 1e-10
    assert abs(put_leg["model_price"] - black76(F, K, r, vol, T, False)) < 1e-10
    # put-call parity on Black-76
    assert abs(call_leg["model_price"] - put_leg["model_price"] - math.exp(-r * T) * (F - K)) < 1e-10

    h = 1e-4
    fd_delta = (black76(F + h, K, r, vol, T, True) - black76(F - h, K, r, vol, T, True)) / (2 * h)
    fd_vega = (black76(F, K, r, vol + h, T, True) - black76(F, K, r, vol - h, T, True)) / (2 * h) / 100
    fd_theta = -(black76(F, K, r, vol, T + h, True) - black76(F, K, r, vol, T - h, True)) / (2 * h) / 365
    assert abs(call_leg["delta"] - fd_delta) < 1e-6
    assert abs(call_leg["vega"] - fd_vega) < 1e-6
    assert abs(call_leg["theta"] - fd_theta) < 1e-6
    assert abs(put_leg["delta"] - (fd_delta - math.exp(-r * T))) < 1e-6


def test_supplied_greeks_kept_when_compute_greeks_false():
    chain = [opt("call", 110)]
    chain[0].update(delta=0.123, gamma=0.0, theta=-0.5, vega=0.2)
    _, rows = ok(screen(chain, ["single_calls"], compute_greeks=False, multiplier=100))
    assert abs(rows[0]["net_delta"] - 12.3) < 1e-12


# ── Fixed behaviours from the original screener ──────────────────────────────

def test_iron_condor_max_loss_uses_wider_wing():
    chain = [
        opt("call", 110, bid=2.0, ask=2.0), opt("call", 115, bid=1.0, ask=1.0),
        opt("put", 90, bid=2.0, ask=2.0), opt("put", 80, bid=0.5, ask=0.5),
    ]
    summary, rows = ok(screen(chain, ["iron_condors"]))
    assert summary["n_generated"] == 1
    ic = rows[0]
    assert [l["qty"] for l in ic["legs"]] == [-1, 1, -1, 1]
    assert abs(ic["credit"] - 4.0) < 1e-12 and abs(ic["debit"] - 1.5) < 1e-12
    assert abs(ic["max_gain"] - 2.5) < 1e-12           # net credit, not gross
    assert abs(ic["max_loss"] - (10.0 - 2.5)) < 1e-12  # put wing (10) is wider than call wing (5)


def test_reverse_iron_condor_short_direction():
    chain = [
        opt("call", 110, bid=2.0, ask=2.0), opt("call", 115, bid=1.0, ask=1.0),
        opt("put", 90, bid=2.0, ask=2.0), opt("put", 80, bid=0.5, ask=0.5),
    ]
    _, rows = ok(screen(chain, ["iron_condors"], strategy_filter={"direction": "SHORT"}))
    ic = rows[0]
    assert [l["qty"] for l in ic["legs"]] == [1, -1, 1, -1]
    assert abs(ic["max_loss"] - 2.5) < 1e-12
    assert abs(ic["max_gain"] - (10.0 - 2.5)) < 1e-12


def test_short_single_call_has_unbounded_loss():
    chain = [opt("call", 110, bid=3.0, ask=3.5)]
    _, rows = ok(screen(chain, ["single_calls"], strategy_filter={"direction": "SHORT"}))
    s = rows[0]
    assert s["legs"][0]["qty"] == -1
    assert abs(s["max_gain"] - 3.0) < 1e-12
    assert s["max_loss"] is None and s["max_loss_unbounded"] is True


def test_missing_direction_defaults_to_long():
    _, rows = ok(screen([opt("call", 110)], ["single_calls"], strategy_filter={"direction": None}))
    assert rows[0]["direction"] == "LONG"


def test_rank_by_credit():
    chain = [opt("call", 110 + i, bid=1.0 + i, ask=1.1 + i) for i in range(3)]
    _, rows = ok(screen(chain, ["single_calls"], rank="credit", strategy_filter={"direction": "SHORT"}))
    assert [r["credit"] for r in rows] == sorted((r["credit"] for r in rows), reverse=True)


# ── Crypto-specific behaviour ─────────────────────────────────────────────────

def test_executable_prices_buy_ask_sell_bid():
    chain = [opt("call", 100, bid=5.0, ask=5.5), opt("put", 100, bid=4.0, ask=4.25)]
    _, rows = ok(screen(chain, ["straddles"]))
    assert abs(rows[0]["cost"] - 9.75) < 1e-12
    _, rows = ok(screen(chain, ["straddles"], strategy_filter={"direction": "SHORT"}))
    assert abs(rows[0]["credit"] - 9.0) < 1e-12


def test_one_sided_quotes():
    chain = [opt("call", 100, bid=None, ask=5.5), opt("put", 100, bid=4.0, ask=4.25)]
    summary, _ = ok(screen(chain, ["straddles"]))
    assert summary["n_options_after_filter"] == 1          # dropped by require_two_sided

    summary, _ = ok(screen(chain, ["straddles"], option_filter={"require_two_sided": False},
                           strategy_filter={"direction": "SHORT"}))
    assert summary["n_generated"] == 1 and summary["n_passed"] == 0   # cannot sell without a bid


def test_otm_uses_each_expiry_forward():
    chain = [
        opt("call", 105, expiry="NEAR", years=0.05, forward=100.0),
        opt("call", 105, expiry="FAR", years=0.5, forward=110.0),    # ITM against its own forward
    ]
    _, rows = ok(screen(chain, ["single_calls"]))
    assert [r["legs"][0]["expiry"] for r in rows] == ["NEAR"]


def test_moneyness_and_spread_pct_filters():
    chain = [opt("call", 120, bid=1.0, ask=1.1), opt("call", 150, bid=1.0, ask=1.1),
             opt("call", 125, bid=1.0, ask=2.0)]
    summary, _ = ok(screen(chain, ["single_calls"],
                           option_filter={"moneyness_range": [1.1, 1.3], "max_bid_ask_spread_pct": 0.2}))
    assert summary["n_options_after_filter"] == 1


def test_multiplier_scales_cost_and_greeks():
    chain = [opt("call", 110, bid=1.0, ask=1.2)]
    _, one = ok(screen(chain, ["single_calls"]))
    _, ten = ok(screen(chain, ["single_calls"], multiplier=10))
    assert abs(ten[0]["cost"] - 10 * one[0]["cost"]) < 1e-12
    assert abs(ten[0]["net_delta"] - 10 * one[0]["net_delta"]) < 1e-12


def test_forward_vol_calendar():
    chain = [
        opt("call", 100, expiry="E1", years=0.1, iv=0.5, bid=2.0, ask=2.1),
        opt("call", 100, expiry="E2", years=0.3, iv=0.6, bid=5.0, ask=5.2),
    ]
    _, rows = ok(screen(chain, ["forward_vols"], rank="forward_vol"))
    s = rows[0]
    expected = math.sqrt((0.36 * 0.3 - 0.25 * 0.1) / 0.2)
    assert abs(s["forward_vol"] - expected) < 1e-12
    assert [(l["expiry"], l["qty"]) for l in s["legs"]] == [("E1", -1), ("E2", 1)]
    assert abs(s["cost"] - (5.2 - 2.0)) < 1e-12 and abs(s["max_loss"] - 3.2) < 1e-12
    assert s["max_gain"] is None

    _, rows = ok(screen(chain, ["forward_vols"], strategy_filter={"forward_vol_range": [0.9, 1.0]}))
    assert rows == []


# ── Model vol / edge ──────────────────────────────────────────────────────────

def test_edge_with_flat_and_none_model():
    F, T = 100.0, 0.25
    chain = [opt("call", 110, forward=F, years=T, bid=1.0, ask=1.2, iv=0.6)]
    _, rows = ok(screen(chain, ["single_calls"], model_vol="flat", model_vol_flat=0.8))
    s = rows[0]
    model = black76(F, 110, 0.0, 0.8, T, True)
    assert abs(s["model_value"] - model) < 1e-12
    assert abs(s["edge"] - (model - 1.2)) < 1e-12

    _, rows = ok(screen(chain, ["single_calls"], model_vol="none"))
    assert rows[0]["edge"] is None


def test_edge_with_heston_and_surface():
    chain = [opt("call", 110, years=0.5, bid=4.0, ask=4.2, iv=0.6),
             opt("put", 90, years=0.5, bid=3.0, ask=3.2, iv=0.6)]
    r = screen(chain, ["strangles"], model_vol="heston",
               heston={"v0": 0.36, "kappa": 2.0, "theta": 0.36, "xi": 0.3, "rho": -0.3})
    _, rows = ok(r)
    assert rows[0]["model_value"] is not None and rows[0]["model_value"] > 0

    surface = {"strikes": [80, 100, 120, 80, 100, 120], "expiries": [0.25, 0.25, 0.25, 1, 1, 1],
               "ivs": [0.7] * 6}
    _, rows = ok(screen(chain, ["strangles"], model_vol="surface", surface=surface))
    expected = black76(100, 110, 0, 0.7, 0.5, True) + black76(100, 90, 0, 0.7, 0.5, False)
    assert abs(rows[0]["model_value"] - expected) < 1e-9


def test_edge_filter_and_rank():
    chain = [opt("call", 110 + 5 * i, bid=0.5, ask=0.6 + 0.3 * i, iv=0.6) for i in range(4)]
    _, rows = ok(screen(chain, ["single_calls"], rank="edge", model_vol="flat", model_vol_flat=0.6))
    edges = [r["edge"] for r in rows]
    assert edges == sorted(edges, reverse=True)
    _, rows = ok(screen(chain, ["single_calls"], model_vol="flat", model_vol_flat=0.6,
                        strategy_filter={"edge_range": [0, None]}))
    assert all(r["edge"] >= 0 for r in rows)


# ── Ranking / scale / determinism ─────────────────────────────────────────────

def big_chain(n_strikes=40, expiries=3):
    chain = []
    for e in range(expiries):
        F = 100.0 + 2 * e
        for i in range(n_strikes):
            K = 60.0 + i * 2.0
            base = max(F - K, 0.0) + 2.0 + 0.01 * i
            chain.append(opt("call", K, expiry=f"E{e}", years=0.1 * (e + 1), forward=F,
                             bid=base, ask=base + 0.1, iv=0.5 + 0.002 * i))
            base_p = max(K - F, 0.0) + 2.0 + 0.01 * (n_strikes - i)
            chain.append(opt("put", K, expiry=f"E{e}", years=0.1 * (e + 1), forward=F,
                             bid=base_p, ask=base_p + 0.1, iv=0.5 + 0.002 * i))
    return chain


def brute_force_ic_count(chain):
    total = 0
    for e in {o["expiry"] for o in chain}:
        F = next(o["forward"] for o in chain if o["expiry"] == e)
        calls = [o["strike"] for o in chain if o["expiry"] == e and o["option_type"] == "call"]
        puts = [o["strike"] for o in chain if o["expiry"] == e and o["option_type"] == "put"]
        for sc in calls:
            if sc <= F:
                continue
            n_bc = sum(1 for k in calls if k > sc)
            for sp in puts:
                if sp >= F:
                    continue
                total += n_bc * sum(1 for k in puts if k < sp)
    return total


def test_iron_condor_enumeration_count_and_top_n():
    chain = big_chain()
    summary, rows = ok(screen(chain, ["iron_condors"], top_n=25))
    assert summary["n_generated"] == brute_force_ic_count(chain)
    assert summary["n_passed"] == summary["n_generated"]
    assert len(rows) == 25
    costs = [r["cost"] for r in rows]
    assert costs == sorted(costs)                                    # cost ranks ascending by default


def test_deterministic_across_thread_counts():
    chain = big_chain()
    req = dict(top_n=100, rank="rr")
    prev = numerics.get_max_threads()
    try:
        numerics.set_num_threads(1)
        _, single = ok(screen(chain, ["iron_condors", "strangles", "straddles"], **req))
        numerics.set_num_threads(8)
        _, multi = ok(screen(chain, ["iron_condors", "strangles", "straddles"], **req))
    finally:
        numerics.set_num_threads(prev)
    assert [r["label"] for r in single] == [r["label"] for r in multi]


def test_by_kind_counts():
    summary, _ = ok(screen(big_chain(10, 2), ["straddles", "strangles"]))
    kinds = summary["by_kind"]
    assert set(kinds) == {"straddle", "strangle"}
    assert summary["n_generated"] == sum(k["generated"] for k in kinds.values())


# ── Contract / errors ─────────────────────────────────────────────────────────

def test_bad_requests_return_codes():
    chain = [opt("call", 110)]
    cases = [
        (dict(strategy_filter={"rr_range": [1]}), "bad_range_rr_range"),
        (dict(rank="nope"), "bad_rank_key"),
        (dict(model_vol="flat"), "model_vol_flat_required"),
        (dict(price_mode="last"), "bad_price_mode"),
        (dict(strategy_filter={"direction": "UP"}), "bad_direction"),
    ]
    for extra, code in cases:
        r = screen(chain, ["single_calls"], **extra)
        assert r["status"] == "error" and r["error"]["message"] == code, (extra, r.get("error"))

    bad_chain = [dict(opt("call", 110), option_type="future")]
    r = screen(bad_chain, ["single_calls"])
    assert r["error"]["message"] == "bad_option_type"


def test_contract_version_and_buffer_growth():
    chain = big_chain()
    saved = numerics._INITIAL_BUFFER
    try:
        numerics._INITIAL_BUFFER = 4096           # force the grow-and-retry path
        r = screen(chain, ["iron_condors"], top_n=200)
    finally:
        numerics._INITIAL_BUFFER = saved
    summary, rows = ok(r)
    assert r["contract_version"] == CONTRACT_VERSION
    assert len(rows) == 200


if __name__ == "__main__":
    failed = 0
    for name, fn in sorted((n, f) for n, f in globals().items() if n.startswith("test_") and callable(f)):
        try:
            fn()
            print(f"ok    {name}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL  {name}: {e}")
    sys.exit(1 if failed else 0)
