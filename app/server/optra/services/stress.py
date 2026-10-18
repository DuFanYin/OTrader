"""The strategies' holdings under the lab's stress scenarios (GET /api/risk/stress).

Every option a strategy holds (holdings are by contract) is valued at its expiry's forward and its own
implied vol from the newest quotes of its portfolio (the chain the lab reads: chains.load, today's
recording when the engine records), then again with the forward moved by the scenario's spot shock,
the vol by its vol shock and the rate by its shift: Black, by the engine's own function
(quant::black_price through the lab's C ABI). Shares held move with the spot shock. A scenario's P&L
is the change in value, USD. Besides the lab's pack, a spot ladder at unchanged vol, the moves a
same-day option book lives on.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from typing import Any

from optra.infra.numerics import black_batch
from optra.services.stress_library import StressScenario as Scenario, scenarios

from optra.data.days import ET, parse_option
from optra.data import chains

LADDER = (-0.05, -0.02, -0.01, 0.01, 0.02, 0.05)


@dataclass(frozen=True)
class Leg:
    strategy: str
    symbol: str
    quantity: float     # signed contracts
    multiplier: float


def holdings_legs(holdings: dict[str, Any]) -> tuple[list[Leg], list[tuple[str, float, float, float]]]:
    """The option legs and, per strategy, its shares (strategy, quantity, price, multiplier)."""
    legs: list[Leg] = []
    shares = []
    for strategy, h in holdings.items():
        u = h.get("underlying") or {}
        if u.get("quantity"):
            shares.append((strategy, float(u["quantity"]), float(u.get("mid_price") or 0.0),
                           float(u.get("multiplier") or 1.0)))
        for p in (h.get("options") or {}).values():
            if p.get("quantity"):
                legs.append(Leg(strategy, p["symbol"], float(p["quantity"]), float(p.get("multiplier") or 100.0)))
    return legs, shares


def scenario_set(pack: str, severity: str) -> list[Scenario]:
    out = scenarios(pack, severity)
    out += [Scenario(f"Spot {m:+.0%}", f"The underlying {m:+.0%}, vols unchanged.", 1.0 + m) for m in LADDER]
    return out


def stress(holdings: dict[str, Any], pack: str = "core4", severity: str = "moderate",
           chain_of=None) -> dict[str, Any]:
    """chain_of(root) -> the lab chain the legs are marked on (default: the newest of chains.load)."""
    chain_of = chain_of or (lambda root: chains.load(root, None, None))
    scs = scenario_set(pack, severity)
    legs, shares = holdings_legs(holdings)

    markets: dict[str, dict | None] = {}
    warnings: list[str] = []
    priced: list[tuple[Leg, dict, float]] = []   # leg, its chain row, its chain's rate
    unpriced: list[str] = []
    for leg in legs:
        option = parse_option(leg.symbol)
        if option is None:
            unpriced.append(f"{leg.strategy}: {leg.symbol} (not an option symbol)")
            continue
        root, expiry, right, strike = option.root, option.expiry.isoformat(), option.right, option.strike
        if root not in markets:
            try:
                chain = chain_of(root)
                markets[root] = {"chain": chain, "rows": {(r["expiry"], r["option_type"][0].upper(), r["strike"]): r
                                                          for r in chain["chain"]}}
            except (ValueError, OSError) as exc:
                markets[root] = None
                warnings.append(f"no quotes for {root}: {exc}")
        m = markets[root]
        row = m["rows"].get((expiry, right, strike)) if m else None
        if row is None or row["iv"] <= 0 or row["years"] <= 0:
            unpriced.append(f"{leg.strategy}: {leg.symbol} ({'no quote' if row is None else 'no implied vol'})")
            continue
        priced.append((leg, row, m["chain"]["rate"]))

    def values(sc: Scenario | None) -> list[float]:
        if not priced:
            return []
        rate = [r + (sc.rate_shift if sc else 0.0) for _, _, r in priced]
        # one batch per distinct rate (the chains' rates are the same in practice)
        out = [0.0] * len(priced)
        for rt in set(rate):
            idx = [i for i, r in enumerate(rate) if r == rt]
            res = black_batch([priced[i][1]["option_type"] == "call" for i in idx],
                              [priced[i][1]["forward"] * (sc.spot_mult if sc else 1.0) for i in idx],
                              [priced[i][1]["strike"] for i in idx], [priced[i][1]["years"] for i in idx], rt,
                              vols=[max(1e-8, priced[i][1]["iv"] * (sc.vol_mult if sc else 1.0)) for i in idx])
            for i, p in zip(idx, res["prices"], strict=True):
                leg = priced[i][0]
                out[i] = p * leg.quantity * leg.multiplier
        return out

    base = values(None)
    strategies = sorted({leg.strategy for leg, _, _ in priced} | {s[0] for s in shares})
    by_strategy = {s: {"strategy": s, "value": 0.0, "pnl": {}} for s in strategies}
    for (leg, _, _), v in zip(priced, base, strict=True):
        by_strategy[leg.strategy]["value"] += v
    total: dict[str, float] = {}
    for sc in scs:
        shocked = values(sc)
        pnl = {s: 0.0 for s in strategies}
        for (leg, _, _), v0, v1 in zip(priced, base, shocked, strict=True):
            pnl[leg.strategy] += v1 - v0
        for strategy, qty, price, mult in shares:
            pnl[strategy] += qty * price * mult * (sc.spot_mult - 1.0)
        for s in strategies:
            by_strategy[s]["pnl"][sc.name] = pnl[s]
        total[sc.name] = sum(pnl.values())

    worst = min(total, key=total.get) if strategies else None
    return {
        "pack": pack,
        "severity": severity,
        "marked_at": {root: m["chain"]["fetched_at"] for root, m in markets.items() if m},
        "marked_on": {root: m["chain"]["source"] for root, m in markets.items() if m},
        "scenarios": [{"name": s.name, "description": s.description, "spot_mult": s.spot_mult,
                       "vol_mult": s.vol_mult, "rate_shift": s.rate_shift} for s in scs],
        "strategies": list(by_strategy.values()),
        "total": total,
        "worst": {"scenario": worst, "pnl": total[worst]} if worst else None,
        "legs": len(priced),
        "unpriced": unpriced,
        "warnings": warnings + _stale(markets),
    }


def _stale(markets: dict[str, dict | None]) -> list[str]:
    today = datetime.now(ET).date().isoformat()
    return [f"{root} is marked on {m['chain']['source']} quotes of {m['chain']['fetched_at'][:10]}, not today's"
            for root, m in markets.items() if m and m["chain"]["fetched_at"][:10] != today]
