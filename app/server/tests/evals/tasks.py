"""Agent tasks with checkable answers. Each task's truth is computed with the lab's own tools when it is checked, so it
holds for the engine as it is; `live` tasks read the live Deribit chain and need the network."""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Callable

from optra.tools import call_sync

NUMBER = re.compile(r"-?\d[\d,]*\.?\d*(?:[eE][-+]?\d+)?")


def numbers(text: str) -> list[float]:
    """Every number in a text, thousands separators allowed."""
    out = []
    for m in NUMBER.findall(text):
        try:
            out.append(float(m.replace(",", "")))
        except ValueError:
            pass
    return out


def near(tolerance: float) -> Callable[[str, float], bool]:
    """The answer states a number within `tolerance` of the truth."""
    return lambda answer, truth: any(abs(x - truth) <= tolerance for x in numbers(answer))


def contains_all(answer: str, truth: list[str]) -> bool:
    flat = answer.replace(",", "").lower()
    return all(t.lower() in flat for t in truth)


@dataclass(frozen=True)
class Task:
    id: str
    prompt: str
    truth: Callable[[], object]
    check: Callable[[str, object], bool]
    live: bool = False


def _bs_price():
    return call_sync("price_option", spot=100, strike=105, rate=0.03, vol=0.2, maturity=0.5)["result_summary"]["bs"]


def _implied_vol():
    r = call_sync("implied_vol", spot=100, strike=100, rate=0.03, maturity=0.5, market_price=6.5)
    return r["result_summary"]["implied_vol"] * 100


def _american_premium():
    r = call_sync("price_option", spot=100, strike=110, rate=0.05, vol=0.25, maturity=1, is_american=True, option_type="put")
    return r["result_summary"]["american_methods"]["early_exercise_premium"]


def _validation_fix():
    r = call_sync("validate", spot=100, strike=100, rate=0.03, vol=0.2, maturity=0.5, pick_ito=True, ito_n=100,
                          ito_function_type="w2_minus_t")
    fix = r["result_details"]["explainable_qa"]["minimal_fix_set"][0]
    return ["ito_n", str(fix["proposed_value"])]


def _heston_price():
    r = call_sync("heston_price", spot=100, strike=100, rate=0.03, maturity=1, v0=0.04, kappa=1.5, theta=0.04, xi=0.5, rho=-0.7)
    return r["result_summary"]["heston_price"]


def _best_strangle():
    r = call_sync("screen_strategies", currency="BTC", strategies={"strangles": True},
                                   option_filter={"days_to_expiry_range": [0, 30]}, rank={"key": "edge", "top_n": 1})
    legs = r["result_details"]["strategies"][0]["legs"]
    return [str(int(leg["strike"])) for leg in legs]


TASKS = [
    Task("bs_price",
         "What is the Black-Scholes price of a European call with spot 100, strike 105, six months to expiry, 20% "
         "volatility and a 3% rate, no dividends? Give it to four decimals.",
         _bs_price, near(5e-4)),
    Task("implied_vol",
         "A six-month at-the-money call on a stock at 100 trades at 6.50, and the rate is 3%. What is its implied "
         "volatility, in percent to two decimals?",
         _implied_vol, near(0.02)),
    Task("american_premium",
         "How much is the right to exercise early worth for a one-year American put with spot 100, strike 110, 25% "
         "volatility and a 5% rate? Give the early-exercise premium to two decimals.",
         _american_premium, near(0.02)),
    Task("validation_fix",
         "Check Itô's formula for W² − t on discrete paths of only 100 steps with the lab's validation gate. Which parameter "
         "does the lab suggest changing, and to what value?",
         _validation_fix, contains_all),
    Task("heston_price",
         "Price a one-year at-the-money call (spot 100, strike 100, rate 3%) in the Heston model with v0 = 0.04, "
         "kappa = 1.5, theta = 0.04, xi = 0.5 and rho = −0.7. Give the price to three decimals.",
         _heston_price, near(2e-3)),
    Task("best_strangle",
         "On the live BTC option chain, which long strangle expiring within 30 days has the highest edge against Deribit's "
         "marks? Give its two strikes.",
         _best_strangle, contains_all, live=True),
]
