"""The inputs of every tool. Each field says what it means, its unit and its range: the descriptions are what an agent
reads at /api/tools and what the API documentation shows."""

from typing import Annotated, Literal

from pydantic import AfterValidator, BaseModel, ConfigDict, Field

from optra.services.market_data import chain_underlyings

# Shared field definitions: the same quantity reads the same everywhere.
SPOT = "Price of the underlying now, in currency units (e.g. USD for BTC)."
STRIKE = "Strike price, in the same units as spot."
RATE = "Risk-free rate, continuously compounded, as a decimal per year (0.05 = 5%)."
VOL = "Volatility of the underlying, annualised, as a decimal (0.2 = 20%)."
MATURITY = "Time to expiry in years (0.25 = three months)."
DIVIDEND = "Continuous dividend yield (in FX mode the foreign rate), as a decimal per year."
OPTION_TYPE = "call or put."
MC_PATHS = "Monte Carlo paths; the Monte Carlo standard error shrinks like 1/sqrt(paths)."


class Input(BaseModel):
    """A tool's input: an argument it does not know is an error, so a misspelt name is never silently ignored."""
    model_config = ConfigDict(extra="forbid")


class NoInput(Input):
    """A tool that takes no input."""


class PricingRequest(Input):
    spot: float = Field(..., gt=0, description=SPOT)
    strike: float = Field(..., gt=0, description=STRIKE)
    rate: float = Field(..., ge=-1.0, le=1.0, description=RATE)
    vol: float = Field(..., gt=0, le=5.0, description=VOL)
    maturity: float = Field(..., gt=0, le=100.0, description=MATURITY)
    n_paths: int = Field(10000, ge=100, le=20_000_000, description=MC_PATHS)
    product_type: Literal["european_call", "european_put", "digital_call"] = Field(
        "european_call", description="digital_call prices a cash-or-nothing call (Black-Scholes only); otherwise option_type decides.")
    dividend_yield: float = Field(0.0, ge=-1.0, le=1.0, description=DIVIDEND)
    fx_mode: bool = Field(False, description="Price an FX option (Garman-Kohlhagen): dividend_yield is the foreign rate.")
    numeraire: Literal["money_market", "stock"] = Field(
        "money_market", description="Reported with the result; it does not change the price.")
    is_american: bool = Field(False, description=(
        "American exercise: adds binomial, trinomial, finite-difference (PSOR) and Longstaff-Schwartz prices and the "
        "early-exercise premium."))
    mc_sampler: Literal["pseudorandom", "antithetic", "sobol"] = Field(
        "pseudorandom", description="Monte Carlo sampler: antithetic and sobol reduce the error for the same paths.")
    option_type: Literal["call", "put"] = Field("call", description=OPTION_TYPE)
    lsm_paths: int = Field(0, ge=0, le=2_000_000, description="Longstaff-Schwartz paths for American options; 0 derives them from n_paths.")
    lsm_steps: int = Field(50, ge=1, le=1000, description="Longstaff-Schwartz exercise dates.")


class PricingBatchRequest(Input):
    jobs: list[PricingRequest] = Field(..., min_length=1, max_length=500, description="Options to price, each priced every way.")


class PricingBatchGridRequest(Input):
    """Build a spot/vol shock grid server-side from a single base spec."""
    base: PricingRequest = Field(..., description="The option at the centre of the grid.")
    n_jobs: int = Field(16, ge=1, le=500, description="Points in the grid.")
    spot_shock: float = Field(0.02, ge=0.0, le=1.0, description="Spot step between grid points, as a fraction of spot.")


class LegSpecRequest(Input):
    option_type: Literal["call", "put"] = Field("call", description=OPTION_TYPE)
    strike: float = Field(..., gt=0, description=STRIKE)
    quantity: float = Field(1.0, ge=-100.0, le=100.0, description="Contracts; negative sells.")
    # Optional per-leg overrides (e.g. legs handed over from the screener)
    vol: float | None = Field(None, gt=0, le=5.0, description="This leg's own vol; default the position's.")
    maturity: float | None = Field(None, gt=0, le=100.0, description="This leg's own time to expiry in years; default the position's.")
    forward: float | None = Field(None, gt=0, description="This leg's forward: prices it with Black-76 (as for Deribit options).")

    def engine_payload(self) -> dict:
        leg = {"option_type": self.option_type, "strike": self.strike, "quantity": self.quantity}
        for key in ("vol", "maturity", "forward"):
            value = getattr(self, key)
            if value is not None:
                leg[key] = value
        return leg


class HedgingRequest(Input):
    spot: float = Field(..., gt=0, description=SPOT)
    strike: float = Field(..., gt=0, description=STRIKE)
    rate: float = Field(..., ge=-1.0, le=1.0, description=RATE)
    vol: float = Field(..., gt=0, le=5.0, description=VOL)
    maturity: float = Field(..., gt=0, le=100.0, description=MATURITY)
    n_rebalances: int = Field(52, ge=1, le=10_000, description="Hedge rebalances until expiry (52 = weekly over a year).")
    n_paths: int = Field(500, ge=50, le=200_000, description="Simulated paths; every strategy is run on the same ones.")
    transaction_cost_bps: float = Field(5.0, ge=0.0, le=1000.0, description="Cost of trading the hedge, in basis points of the traded value.")
    rebalance_threshold: float = Field(0.02, ge=0.0, le=1.0, description="Threshold strategy: rebalance only when delta has moved by more than this.")
    vol_mismatch_mult: float = Field(1.15, ge=0.1, le=5.0, description="Mismatch strategy: hedge with vol × this while the market moves with vol.")
    dividend_yield: float = Field(0.0, ge=-1.0, le=1.0, description=DIVIDEND)
    option_type: Literal["call", "put"] = Field("call", description="Without legs: the option sold at strike, a call or a put.")
    legs: list[LegSpecRequest] | None = Field(None, min_length=1, max_length=10, description=(
        "A position to hedge instead of one option sold at strike: the legs as held (quantity > 0 bought, < 0 sold), each "
        "with its own vol, maturity or forward if given."))


class SimulationRequest(Input):
    model: Literal["brownian", "vasicek"] = Field("brownian", description="brownian (dX = sigma dW) or vasicek (dX = kappa (theta − X) dt + sigma dW).")
    n_steps: int = Field(100, ge=1, le=500_000, description="Time steps.")
    dt: float = Field(0.01, gt=0, le=10.0, description="Length of a step, in years.")
    sigma: float = Field(0.2, gt=0, le=5.0, description="Volatility of the process.")
    kappa: float = Field(1.2, ge=0, le=100.0, description="Vasicek: speed of mean reversion, per year.")
    theta: float = Field(0.03, ge=-10.0, le=10.0, description="Vasicek: the long-run mean.")
    x0: float = Field(0.0, description="Starting value.")


class StatsRequest(Input):
    mu: float = Field(0.0, description="Mean of the normal distribution.")
    sigma: float = Field(1.0, gt=0, le=100.0, description="Standard deviation of the normal distribution.")
    theta: float = Field(1.0, description="Argument of the moment-generating function E[exp(theta X)].")
    sample_size: int = Field(10000, ge=10, le=10_000_000, description="Draws; the sample moments converge like 1/sqrt(n).")


ITO_FUNCTION = "exp_martingale = exp(theta W − theta² t / 2), w2_minus_t = W² − t, or w3 = W³."


class ItoCheckRequest(Input):
    function_type: Literal["exp_martingale", "w2_minus_t", "w3"] = Field("exp_martingale", description=ITO_FUNCTION)
    theta: float = Field(0.7, description="theta of the exponential martingale.")
    t: float = Field(1.0, gt=0, le=100.0, description="Horizon, in years.")
    n_steps: int = Field(2000, ge=100, le=2_000_000, description="Steps of the discrete paths Itô's formula is checked on.")


class MeasureChangeRequest(Input):
    mu: float = Field(0.08, description="Drift under the physical measure P, decimal per year.")
    r: float = Field(0.02, description="Risk-free rate, the drift under the risk-neutral measure Q.")
    sigma: float = Field(0.2, gt=0, le=5.0, description=VOL)
    t: float = Field(1.0, gt=0, le=100.0, description="Horizon, in years.")
    n_steps: int = Field(200, ge=20, le=2_000_000, description="Steps of the density path.")


class MeasureCompareRequest(Input):
    mu: float = Field(0.08, description="Drift under the physical measure P, decimal per year.")
    r: float = Field(0.02, description="Risk-free rate, the drift under the risk-neutral measure Q.")
    sigma: float = Field(0.2, gt=0, le=5.0, description=VOL)
    t: float = Field(1.0, gt=0, le=100.0, description="Horizon, in years.")
    n_steps: int = Field(400, ge=40, le=200_000, description="Steps per path.")
    n_paths: int = Field(2000, ge=100, le=200_000, description="Paths under each measure (the same shocks).")
    x0: float = Field(1.0, description="Starting value of the price.")


class PdeRequest(Input):
    spot: float = Field(..., gt=0, description=SPOT)
    strike: float = Field(..., gt=0, description=STRIKE)
    rate: float = Field(..., ge=-1.0, le=1.0, description=RATE)
    vol: float = Field(..., gt=0, le=5.0, description=VOL)
    maturity: float = Field(..., gt=0, le=100.0, description=MATURITY)
    dividend_yield: float = Field(0.0, ge=-1.0, le=1.0, description=DIVIDEND)
    s_steps: int = Field(200, ge=40, le=2000, description="Grid points in the price direction.")
    t_steps: int = Field(200, ge=40, le=4000, description="Grid points in time (refined automatically when an explicit scheme would be unstable).")
    method: Literal["crank_nicolson", "implicit", "explicit"] = Field("crank_nicolson", description="Finite-difference scheme.")
    option_type: Literal["call", "put"] = Field("call", description=OPTION_TYPE)
    is_american: bool = Field(False, description="American exercise, solved by PSOR.")


LADDER = "Lattice step counts to price at, ascending."


class ConvergenceRequest(Input):
    spot: float = Field(..., gt=0, description=SPOT)
    strike: float = Field(..., gt=0, description=STRIKE)
    rate: float = Field(..., ge=-1.0, le=1.0, description=RATE)
    vol: float = Field(..., gt=0, le=5.0, description=VOL)
    maturity: float = Field(..., gt=0, le=100.0, description=MATURITY)
    dividend_yield: float = Field(0.0, ge=-1.0, le=1.0, description=DIVIDEND)
    step_ladder: list[int] = Field(default_factory=lambda: [10, 20, 40, 80, 120, 200, 320, 500], min_length=2, max_length=30, description=LADDER)
    option_type: Literal["call", "put"] = Field("call", description=OPTION_TYPE)


class BenchmarkRequest(Input):
    pricing: PricingRequest = Field(..., description="The option to price with every method.")
    pde_method: Literal["crank_nicolson", "implicit", "explicit"] = Field("crank_nicolson", description="Finite-difference scheme.")
    pde_s_steps: int = Field(160, ge=40, le=2000, description="Finite-difference grid points in price.")
    pde_t_steps: int = Field(160, ge=40, le=4000, description="Finite-difference grid points in time.")
    benchmark_baseline: Literal["pde", "bs", "mc", "binomial", "trinomial"] = Field("pde", description="The method the others are measured against.")


class ValidationGateRequest(Input):
    pick_stats: bool = Field(False, description="Check sample moments against the normal distribution.")
    pick_ito: bool = Field(False, description="Check E[f(W_t)] and Itô's formula on discrete paths.")
    pick_simulation: bool = Field(False, description="Check that a simulated path stays finite and the delta-hedge P&L spread is sane.")
    pick_lattice: bool = Field(False, description="Check the trinomial tree against Black-Scholes and LSM / PDE against a binomial American price.")
    compute_block_on_validation: bool = Field(False, description="Report a failed gate as a block rather than a warning.")

    spot: float = Field(..., gt=0, description=SPOT)
    strike: float = Field(..., gt=0, description=STRIKE)
    rate: float = Field(..., ge=-1.0, le=1.0, description=RATE)
    vol: float = Field(..., gt=0, le=5.0, description=VOL)
    maturity: float = Field(..., gt=0, le=100.0, description=MATURITY)

    mu: float = Field(0.0, description="Stats check: mean of the normal distribution.")
    stats_theta: float = Field(1.0, description="Stats check: argument of the moment-generating function.")
    stats_n: int = Field(10000, ge=10, le=10_000_000, description="Stats check: sample size.")

    ito_function_type: Literal["exp_martingale", "w2_minus_t", "w3"] = Field("exp_martingale", description="Itô check: " + ITO_FUNCTION)
    ito_theta: float = Field(0.7, description="Itô check: theta of the exponential martingale.")
    ito_t: float = Field(1.0, gt=0, le=100.0, description="Itô check: horizon, in years.")
    ito_n: int = Field(2000, ge=100, le=2_000_000, description="Itô check: steps of the discrete paths.")

    sim_steps: int = Field(100, ge=1, le=500_000, description="Simulation check: steps of the GBM path (dt 0.01).")

    n_rebalances: int = Field(52, ge=1, le=10_000, description="Hedging check: rebalances until expiry.")
    hedge_paths: int = Field(500, ge=50, le=200_000, description="Hedging check: paths.")

GREEK = "delta, gamma, vega, theta or rho (Black-Scholes)."


class GreekSurfaceRequest(Input):
    strike: float = Field(..., gt=0, description=STRIKE)
    rate: float = Field(..., ge=-1.0, le=1.0, description=RATE)
    vol: float = Field(..., gt=0, le=5.0, description=VOL)
    dividend_yield: float = Field(0.0, ge=-1.0, le=1.0, description=DIVIDEND)
    spot_min: float = Field(..., gt=0, description="Lowest spot of the grid.")
    spot_max: float = Field(..., gt=0, description="Highest spot of the grid.")
    mat_min: float = Field(0.1, gt=0, le=100.0, description="Shortest maturity of the grid, in years.")
    mat_max: float = Field(3.0, gt=0, le=100.0, description="Longest maturity of the grid, in years.")
    n_spots: int = Field(21, ge=5, le=50, description="Spot points.")
    n_mats: int = Field(11, ge=5, le=30, description="Maturity points.")
    greek: Literal["delta", "gamma", "vega", "theta", "rho"] = Field("delta", description=GREEK)


class ImpliedVolRequest(Input):
    market_price: float = Field(..., gt=0, description="Observed price of a European call, in the units of spot.")
    spot: float = Field(..., gt=0, description=SPOT)
    strike: float = Field(..., gt=0, description=STRIKE)
    rate: float = Field(..., ge=-1.0, le=1.0, description=RATE)
    maturity: float = Field(..., gt=0, le=100.0, description=MATURITY)
    dividend_yield: float = Field(0.0, ge=-1.0, le=1.0, description=DIVIDEND)


class ImpliedVolBatchRequest(Input):
    market_prices: list[float] = Field(..., min_length=1, max_length=500, description="Observed European call prices.")
    strikes: list[float] = Field(..., min_length=1, max_length=500, description="Their strikes, in the same order.")
    expiries: list[float] = Field(..., min_length=1, max_length=500, description="Their times to expiry in years, in the same order.")
    spot: float = Field(..., gt=0, description=SPOT)
    rate: float = Field(..., ge=-1.0, le=1.0, description=RATE)
    dividend_yield: float = Field(0.0, ge=-1.0, le=1.0, description=DIVIDEND)


V0, KAPPA, THETA, XI, RHO = ("Heston: initial variance (vol², 0.04 = 20% vol).", "Heston: speed of mean reversion of the variance.",
                             "Heston: long-run variance.", "Heston: volatility of the variance.",
                             "Heston: correlation between the price and its variance.")


class HestonPriceRequest(Input):
    spot: float = Field(..., gt=0, description=SPOT)
    strike: float = Field(..., gt=0, description=STRIKE)
    rate: float = Field(..., ge=-1.0, le=1.0, description=RATE)
    maturity: float = Field(..., gt=0, le=100.0, description=MATURITY)
    v0: float = Field(0.04, gt=0, description=V0)
    kappa: float = Field(1.5, gt=0, description=KAPPA)
    theta: float = Field(0.04, gt=0, description=THETA)
    xi: float = Field(0.5, gt=0, description=XI)
    rho: float = Field(-0.7, ge=-1.0, le=1.0, description=RHO)


class HestonCalibrateRequest(Input):
    spot: float = Field(..., gt=0, description=SPOT)
    rate: float = Field(..., ge=-1.0, le=1.0, description=RATE)
    dividend_yield: float = Field(0.0, ge=-1.0, le=1.0, description=DIVIDEND)
    market_strikes: list[float] = Field(..., min_length=2, max_length=200, description="Strikes of the quoted European calls.")
    market_maturities: list[float] = Field(..., min_length=2, max_length=200, description="Their times to expiry in years, in the same order.")
    market_prices: list[float] = Field(..., min_length=2, max_length=200, description="Their prices, in the same order.")
    init_v0: float = Field(0.04, gt=0, description="Starting guess: " + V0)
    init_kappa: float = Field(1.5, gt=0, description="Starting guess: " + KAPPA)
    init_theta: float = Field(0.04, gt=0, description="Starting guess: " + THETA)
    init_xi: float = Field(0.5, gt=0, description="Starting guess: " + XI)
    init_rho: float = Field(-0.7, ge=-1.0, le=1.0, description="Starting guess: " + RHO)
    max_iter: int = Field(500, ge=50, le=5000, description="Nelder-Mead iterations at most.")


class MultiLegRequest(Input):
    spot: float = Field(..., gt=0, description=SPOT)
    rate: float = Field(..., ge=-1.0, le=1.0, description=RATE)
    vol: float = Field(..., gt=0, le=5.0, description="Vol of every leg without its own.")
    maturity: float = Field(..., gt=0, le=100.0, description="Time to expiry of every leg without its own, in years.")
    dividend_yield: float = Field(0.0, ge=-1.0, le=1.0, description=DIVIDEND)
    n_paths: int = Field(10000, ge=100, le=20_000_000, description=MC_PATHS)
    legs: list[LegSpecRequest] = Field(..., min_length=1, max_length=10, description="The position's legs.")


class StressLibraryRequest(Input):
    spot: float = Field(..., gt=0, description=SPOT)
    strike: float = Field(..., gt=0, description=STRIKE + " Ignored when legs are given.")
    rate: float = Field(..., ge=-1.0, le=1.0, description=RATE)
    vol: float = Field(..., gt=0, le=5.0, description=VOL)
    maturity: float = Field(..., gt=0, le=100.0, description=MATURITY)
    n_paths: int = Field(10000, ge=100, le=20_000_000, description=MC_PATHS)
    dividend_yield: float = Field(0.0, ge=-1.0, le=1.0, description=DIVIDEND)
    n_rebalances: int = Field(52, ge=1, le=10_000, description="Hedge comparison: rebalances until expiry.")
    hedge_paths: int = Field(1000, ge=50, le=200_000, description="Hedge comparison: paths.")
    transaction_cost_bps: float = Field(5.0, ge=0.0, le=1000.0, description="Hedge comparison: trading cost, basis points.")
    rebalance_threshold: float = Field(0.02, ge=0.0, le=1.0, description="Hedge comparison: delta move that triggers the threshold strategy.")
    vol_mismatch_mult: float = Field(1.15, ge=0.1, le=5.0, description="Hedge comparison: the mismatch strategy hedges with vol × this.")
    option_type: Literal["call", "put"] = Field("call", description="The single option's type (ignored when legs are given).")
    stress_pack: str = Field("core4", description="Scenario pack: core4, vol_first, rates_first or crash_kit (the stress_packs tool lists them).")
    stress_severity: Literal["mild", "moderate", "severe"] = Field("moderate", description="Shock size: mild 0.7×, moderate 1×, severe 1.5×.")
    include_hedge_compare: bool = Field(True, description="Also run the delta-hedging comparison in every scenario.")
    # Portfolio mode: when legs are given, every scenario reprices the whole
    # multi-leg position instead of a single option at `strike`.
    legs: list[LegSpecRequest] | None = Field(None, min_length=1, max_length=10, description="A position to stress instead of one option at strike.")


# ── Screener ──────────────────────────────────────────────────────────────────

Range = tuple[float | None, float | None] | None
CURRENCY = ("BTC or ETH: the Deribit option chain to use; or an underlying the trading engine has recorded or "
            "backtest days of (SPXW, SPY), where the server has them (chain_snapshots lists their days).")
AT = "On a recorded day's chain (snapshot_id): the New York time, HH:MM (or a full ISO time); omitted, the day's last quotes."


def _underlying(v):
    v = v.upper() if isinstance(v, str) else v
    if v not in chain_underlyings():
        raise ValueError(f"one of {', '.join(chain_underlyings())}")
    return v


Currency = Annotated[str, AfterValidator(_underlying)]


class ScreenerStrategies(Input):
    single_calls: bool = Field(False, description="Single out-of-the-money calls.")
    iron_condors: bool = Field(False, description="Iron condors (SHORT direction: reverse iron condors).")
    straddles: bool = Field(False, description="Straddles: a call and a put at one strike.")
    strangles: bool = Field(False, description="Strangles: an OTM call and an OTM put.")
    forward_vols: bool = Field(False, description="Same-strike calendars, ranked by the forward vol between their expiries.")


class ScreenerOptionFilter(Input):
    min_volume: float | None = Field(None, ge=0, description="Least traded volume, in coins.")
    min_oi: float | None = Field(None, ge=0, description="Least open interest, in coins.")
    min_price: float | None = Field(None, ge=0, description="Least option price, USD.")
    expiry: str | None = Field(None, description="Only this expiry, as Deribit writes it (e.g. 26DEC26).")
    days_to_expiry_range: Range = Field(None, description="[least, most] days to expiry; null leaves an end open.")
    volume_ratio_range: Range = Field(None, description="[least, most] volume / open interest.")
    max_bid_ask_spread: float | None = Field(None, ge=0, description="Widest bid-ask spread, USD.")
    max_bid_ask_spread_pct: float | None = Field(None, ge=0, description="Widest bid-ask spread as a fraction of the mid.")
    moneyness_range: Range = Field(None, description="[least, most] strike / forward.")
    require_two_sided: bool = Field(True, description="Only options quoted on both sides.")


RANGE = "[least, most]; null leaves an end open."


class ScreenerStrategyFilter(Input):
    direction: Literal["LONG", "SHORT"] = Field("LONG", description="LONG buys the strategy, SHORT sells it.")
    debit_range: Range = Field(None, description="Net debit paid, USD: " + RANGE)
    credit_range: Range = Field(None, description="Net credit received, USD: " + RANGE)
    potential_gain_range: Range = Field(None, description="Maximum gain, USD: " + RANGE)
    potential_loss_range: Range = Field(None, description="Maximum loss, USD: " + RANGE)
    rr_range: Range = Field(None, description="Reward / risk ratio: " + RANGE)
    net_delta_range: Range = Field(None, description="Net delta: " + RANGE)
    net_theta_range: Range = Field(None, description="Net theta, USD per day: " + RANGE)
    net_vega_range: Range = Field(None, description="Net vega, USD per vol point: " + RANGE)
    iv_range: Range = Field(None, description="Average implied vol of the legs, decimal: " + RANGE)
    forward_vol_range: Range = Field(None, description="Forward vol (calendars), decimal: " + RANGE)
    edge_range: Range = Field(None, description="Edge = model value − cost, USD: " + RANGE)


class ScreenerHeston(Input):
    v0: float = Field(..., gt=0, description=V0)
    kappa: float = Field(..., gt=0, description=KAPPA)
    theta: float = Field(..., gt=0, description=THETA)
    xi: float = Field(..., gt=0, description=XI)
    rho: float = Field(..., ge=-1.0, le=1.0, description=RHO)


class ScreenerRank(Input):
    key: Literal["rr", "gain", "loss", "cost", "credit", "edge", "forward_vol"] = Field("rr", description="What to rank by.")
    descending: bool | None = Field(None, description="Rank order; null uses the key's natural order (best first).")
    top_n: int = Field(20, ge=1, le=500, description="Strategies to return.")


class ScreenerRequest(Input):
    currency: Currency = Field("BTC", description=CURRENCY)
    snapshot_id: str | None = Field(None, description="A cached chain (chain_snapshots lists them) instead of the live one.")
    at: str | None = Field(None, description=AT)
    strategies: ScreenerStrategies = Field(..., description="Which strategy types to build; at least one.")
    option_filter: ScreenerOptionFilter = Field(ScreenerOptionFilter(), description="Which options may be legs.")
    strategy_filter: ScreenerStrategyFilter = Field(ScreenerStrategyFilter(), description="Which strategies to keep.")
    price_mode: Literal["executable", "mid"] = Field("executable", description="executable buys at the ask and sells at the bid; mid uses mids.")
    model_vol: Literal["mark", "dvol", "flat", "surface", "heston", "none"] = Field("mark", description=(
        "The model a strategy's value (and edge = value − cost) comes from: mark (Deribit's marks), dvol (the DVOL index as "
        "a flat vol), flat (model_vol_flat), surface (an IV surface from the same chain), heston (heston, or calibrated to "
        "the chain when not given), none."))
    model_vol_flat: float | None = Field(None, gt=0, le=5.0, description="The vol for model_vol=flat, decimal.")
    heston: ScreenerHeston | None = Field(None, description="Heston parameters for model_vol=heston; omit to calibrate them to the chain.")
    rate: float | None = Field(None, ge=-1.0, le=1.0, description="Rate for pricing; null uses the chain's own (Deribit's).")
    rank: ScreenerRank = Field(ScreenerRank(), description="Ranking and how many to return.")


class ChainRequest(Input):
    currency: Currency = Field("BTC", description=CURRENCY)
    max_days: float | None = Field(None, gt=0, description="Only expiries within this many days.")
    snapshot_id: str | None = Field(None, description="A cached chain (chain_snapshots lists them) instead of the live one.")
    at: str | None = Field(None, description=AT)


class SnapshotsRequest(Input):
    currency: Currency | None = Field(None, description="Only this currency's snapshots.")


class IvSurfaceRequest(Input):
    spot: float = Field(..., gt=0, description="BTC spot, USD: the centre of the moneyness grid.")
    strike: float = Field(..., gt=0, description="The strike to interpolate the implied vol at.")
    maturity: float = Field(..., gt=0, description="The time to expiry in years to interpolate at, and to centre the grid's maturities on.")
    smile_points: int = Field(17, ge=5, le=21, description="Strikes in the smile of the expiry nearest maturity.")
    moneyness_steps: int = Field(15, ge=5, le=21, description="Grid points from 75% to 125% of spot.")
    tenor_steps: int = Field(11, ge=5, le=21, description="Grid points from 0.4× to 1.8× maturity.")
    burst_lower_quantile: float = Field(0.10, ge=0.0, le=0.49, description="Grid IVs at or below this quantile are marked suppressed.")
    burst_upper_quantile: float = Field(0.90, ge=0.51, le=1.0, description="Grid IVs at or above this quantile are marked burst.")


RUN_ID = "A run's id: the run_id of a result."


class GetRunRequest(Input):
    run_id: str = Field(..., min_length=8, max_length=64, description=RUN_ID)
    rerun: bool = Field(False, description="Also run it again with the same inputs and list what changed (market data may have moved).")


class CompareRunsRequest(Input):
    run_a: str = Field(..., min_length=8, max_length=64, description=RUN_ID + " The first.")
    run_b: str = Field(..., min_length=8, max_length=64, description=RUN_ID + " The second.")


class ListRunsRequest(Input):
    tool: str | None = Field(None, description="Only runs of this tool.")
    limit: int = Field(20, ge=1, le=200, description="Runs to list, newest first.")

