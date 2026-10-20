"""Writing strategies, as tools: what a strategy is written against (strategy_api), writing one and building it
(put_strategy), the classes there are (strategy_classes), building them all again (build_strategies), and installing
a strategy for an account's engine (paper or live), or taking it off, on a person's word (install_strategy,
uninstall_strategy). Where the code lives and who loads it: research/strategies.py.

An agent's way: strategy_api, put_strategy until it builds, then backtest_report or submit_experiment on its classes;
a version that earns it goes to an account through promote_strategy_version, which a person approves.
"""

from __future__ import annotations

import subprocess
from pathlib import Path
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field

from optra import settings
from optra.research import code_identity, ledger, strategies, versions
from optra.tools.base import Result, Tool, ToolError
from optra.utils.files import repo_root

# What a strategy is written against: the base class and its parameters, the market and holdings it reads, the
# numerics it may call, and the base a 0DTE buyer can derive from.
API_HEADERS = {
    "strategy.hpp": "engine/strategy/strategy.hpp",
    "parameters.hpp": "engine/model/parameters.hpp",
    "strategy_registry.hpp": "engine/strategy/strategy_registry.hpp",
    "portfolio.hpp": "engine/model/portfolio.hpp",
    "object.hpp": "engine/model/object.hpp",
    "constant.hpp": "engine/model/constant.hpp",
    "utility.hpp": "engine/model/utility.hpp",
    "order_builder.hpp": "engine/model/order_builder.hpp",
    "engine_api.h": "engine/quant/engine_api.h",
    "zero_dte_buyer.hpp": "strategy/demo/zero_dte_buyer.hpp",
}
EXAMPLE = ("strategy/demo/random_buy.hpp", "strategy/demo/random_buy.cpp")
RULES = [
    "One .cpp file: the class (deriving from strategy::Strategy, inside namespace strategy) and "
    "REGISTER_STRATEGY(ClassName) at the end. #include \"strategy.hpp\" and \"strategy_registry.hpp\"; a 0DTE buyer "
    "may derive from ZeroDteBuyer (#include \"demo/zero_dte_buyer.hpp\").",
    "Parameters: a struct Setting of Param<int> / Param<double> members, one line each {\"name\", default, min, max}; "
    "read them as a member `const Setting setting_ = read_setting<Setting>();` (setting_.name). Take the base class's "
    "constructor: `using Strategy::Strategy;`.",
    "on_day: each trading day's contracts are in; choose the day's chain there (select_chain(min_dte, max_dte)); "
    "send no orders there. on_timer: once a bar (a minute): read quotes, Greeks and the holding, send orders. "
    "on_init, on_stop: optional.",
    "The clock is the market's: now(), minute() (since the 09:30 open: 1 to 390), day(); never count calls.",
    "Contracts, as std::optional values (none when missing): find_chain(symbol) a model::Chain (strikes(), "
    "call(strike), put(strike), atm_strike(), forward(), days_to_expiry()); option_at(*chain, CALL or PUT, strikes "
    "from the money) a model::Option (symbol(), strike(), bid(), ask(), mid(), bid_size(), ask_size(), iv(), and "
    "delta(), gamma(), theta(), vega() of one contract); portfolio()->option(symbol) any option listed; "
    "underlying() (bid(), ask(), mid()). Read them in the callback: they are the market now.",
    "Orders: one kind, units of legs: send_order(legs, price, quantity, type, combo_type), the price the net a unit "
    "(paid positive, received negative; 0 at market). A leg is leg(symbol, BUY or SELL, ratio) for any contract "
    "(an option, the underlying), or combo_legs(STRADDLE, SPREAD, ..., roles (role to option symbol), side) for a standard combo; one leg "
    "at ratio 1 is that contract's own order. close_all_strategy_positions; holds_options(), has_active_order(), "
    "holding(). "
    "register_hedging keeps the delta near zero with the underlying.",
    "One idea, one class: the features it reads are computed in it as it runs, from the quotes, IVs, smiles and "
    "values by day the backtest gives; ComboStrategy is for a combo from the lab and sweeping a structure only. A "
    "question that trades nothing is a strategy that only measures: it writes its answers with write_value(name, "
    "value), one value a name a day (the last written stands; a shape within the day as one value a bucket), and "
    "get_trial gives them back by day.",
    "Code it calls is in its own file or in the engine: not another strategy's .cpp. Each strategy file is a library "
    "of its own, so a base shared by several strategies is a header only (as demo/zero_dte_buyer.hpp).",
    "The flow: put_strategy (a name of lower case letters, digits and _; it builds and returns the class and its "
    "parameters, or the compiler's errors), then backtest (one setting or a space; it waits and answers with the "
    "measures, the control, the deflated Sharpe and what review asks for; replay_trial shows a trial's fills and its "
    "log on its worst days), then register_strategy_version for the best, promote_strategy_version to review, and "
    "holdout_report (once a version). Paper, pilot and live are the person's to approve; the counting, the control, "
    "the robustness checks and the holdout are the system's: an agent's request cannot do them.",
]


class Input(BaseModel):
    model_config = ConfigDict(extra="forbid")


class NoInput(Input):
    pass


class ApiRequest(Input):
    header: Literal[tuple(API_HEADERS)] | None = Field(  # type: ignore[valid-type]
        None, description="One of the headers in full; none: the rules, the base class, its parameters and an example.")


class PutRequest(Input):
    name: str = Field(..., pattern=strategies.NAME.pattern,
                      description="The file's name: strategy/research/<name>.cpp, lower case letters, "
                                  "digits and _.")
    source: str = Field(..., max_length=200_000, description="The whole .cpp (strategy_api says how to write it). "
                                                             "Replaces the file of that name.")


Account = Literal["paper", "live"]


class InstallRequest(Input):
    name: str = Field(..., pattern=strategies.NAME.pattern,
                      description="A strategy file: strategy/research/<name>.cpp.")
    account: Account = Field("paper", description="The account whose engine is to load it.")
    approved: str = Field(..., min_length=3, description="Who puts it on the account and why. A person, not an "
                                                         "agent: an agent's strategy goes through "
                                                         "promote_strategy_version.")


class UninstallRequest(Input):
    name: str = Field(..., pattern=strategies.NAME.pattern,
                      description="A strategy file installed: strategy/<account>/<name>.cpp.")
    account: Account = Field("paper", description="The account it is taken off.")
    reason: str = Field(..., min_length=3, description="Why it is taken off.")


def _read(relative: str) -> str:
    return (repo_root() / relative).read_text()


def strategy_api(p: ApiRequest) -> Result:
    if p.header:
        return Result(summary={"header": p.header, "text": _read(API_HEADERS[p.header])})
    return Result(summary={"rules": RULES, "headers": sorted(API_HEADERS),
                           "strategy.hpp": _read(API_HEADERS["strategy.hpp"]),
                           "parameters.hpp": _read(API_HEADERS["parameters.hpp"]),
                           "example": {Path(f).name: _read(f) for f in EXAMPLE}},
                  notes=["Ask for any header by name (portfolio.hpp: the chains and options; object.hpp: orders, "
                         "fills, holdings)."])


def _built() -> dict[str, Any]:
    """The strategy classes the build has, each with its parameters, its source file and its library; and the
    strategy libraries it could not load, with why."""
    try:
        built = strategies.built_strategies()
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        raise ToolError(f"entry_backtest --list-strategies failed: {exc}", 503) from exc
    root = repo_root()
    return {"strategies": [{"name": c["name"], "parameters": c["parameters"],
                            "source": str(Path(c["source"]).relative_to(root)
                                          if Path(c["source"]).is_relative_to(root) else c["source"]),
                            "demo": strategies.from_demo(c["source"])}
                           for c in built["strategies"]],
            "refused": [{"library": Path(r["library"]).name, "why": r["why"]} for r in built["refused"]]}


def _costs() -> list[dict[str, Any]]:
    """The costs' fields as the page edits them, like a strategy's parameters: a number's default and range, a
    choice's options (ledger.Costs; its risk limits are left to the tools)."""
    out = []
    for name, f in ledger.Costs.model_json_schema()["properties"].items():
        if f.get("type") in ("number", "integer"):
            out.append({"name": name, "default": f["default"], "min": f.get("minimum"), "max": f.get("maximum"),
                        "integer": f["type"] == "integer", "description": f["description"]})
        elif "enum" in f:
            out.append({"name": name, "default": f["default"], "options": f["enum"], "description": f["description"]})
    return out


def strategy_classes(_: NoInput) -> Result:
    return Result(summary={**_built(), "costs": _costs()},
                  notes=["Run one with backtest_report: \"Class:name=value,...\"; write one with put_strategy."])


def _not_in_demo() -> None:
    """A public demo compiles and installs no code a visitor sends."""
    if settings.demo():
        raise ToolError("not in the demo: it builds no strategy a visitor writes", 403)


def put_strategy(p: PutRequest) -> Result:
    _not_in_demo()
    try:
        out = strategies.put(p.name, p.source)
    except ValueError as exc:
        raise ToolError(str(exc), 400) from exc
    if not out["built"]:
        raise ToolError(f"strategy/research/{p.name}.cpp does not build:\n" + "\n".join(out["errors"]), 422)
    notes = ["Backtest it with backtest_report, or try its settings with submit_experiment."]
    if not out["classes"]:
        notes = ["It built, but registered no class: REGISTER_STRATEGY(ClassName) at the end of the file."]
    return Result(summary=out, notes=notes)


def build_strategies(_: NoInput) -> Result:
    _not_in_demo()
    out = strategies.build()
    if not out["built"]:
        raise ToolError("the strategy libraries did not build:\n" + "\n".join(out["errors"]), 422)
    return Result(summary=_built())


def install_strategy(p: InstallRequest) -> Result:
    _not_in_demo()
    source = strategies.source_dir() / f"{p.name}.cpp"
    if not source.exists():
        raise ToolError(f"no strategy/research/{p.name}.cpp", 404)
    research = strategies.library_dir() / f"{p.name}.so"
    try:
        if changed := code_identity.changed_since_built(research):
            raise ValueError(f"{', '.join(changed)} changed since {research.name} was built: put_strategy it again")
        strategies.install(code_identity.strategy_sources(research), p.account)
    except ValueError as exc:
        raise ToolError(str(exc), 422) from exc
    code = code_identity.strategy_hash(research)
    versions.record_install(source.name, code, p.account, True, p.approved.strip())
    return Result(summary={"installed": f"strategy/{p.account}/{source.name}", "code": code},
                  notes=[f"The {p.account} account's engine loads it the next time it lists the strategy classes (the "
                         "Trading page does); a strategy running keeps the code it started with."])


def uninstall_strategy(p: UninstallRequest) -> Result:
    _not_in_demo()
    source = strategies.installed_dir(p.account) / f"{p.name}.cpp"
    if not source.exists():
        raise ToolError(f"no strategy/{p.account}/{p.name}.cpp", 404)
    code = code_identity.strategy_hash(strategies.installed_library(p.account, source.name))
    strategies.uninstall({source.name: ""}, p.account)
    versions.record_install(source.name, code, p.account, False, p.reason.strip())
    return Result(summary={"removed": f"strategy/{p.account}/{source.name}"},
                  notes=["A strategy running on it keeps running until the engine restarts."])


STRATEGY = ("Strategies: C++ files in the repository's strategy/ folder, each built as a library of its own: demo "
            "(the repository's), research (the owner's and agents'), paper and live (installed for an account). "
            "Backtests load demo and research; an account's engine demo and its own. ")

TOOLS = [
    Tool("strategy_api", "How to write a strategy",
         STRATEGY + "What a strategy is written against: the rules, the base class (its callbacks and helpers), how "
         "parameters are declared, and a whole example; or one of the headers in full.",
         ApiRequest, strategy_api, "/backtest/strategies/api", method="GET", engine=False, record=False,
         example={}),
    Tool("put_strategy", "Write a strategy",
         STRATEGY + "Writes strategy/research/<name>.cpp (replacing the file of that name) and builds it alone. "
         "Refused with "
         "the compiler's errors when it does not build; else the classes it registered, with their parameters, ready "
         "for backtest_report and submit_experiment. No engine that trades loads it.",
         PutRequest, put_strategy, "/backtest/strategies/source", cost="seconds", engine=False, record=False,
         example={"name": "example_quiet", "source": '#include "strategy_registry.hpp"\n#include "strategy.hpp"\n\n'
                  "namespace strategy {\n\nclass ExampleQuietStrategy final : public Strategy {\n"
                  "  public:\n    using Strategy::Strategy;\n"
                  "    void on_timer() override {}\n};\n\nREGISTER_STRATEGY(ExampleQuietStrategy);\n\n"
                  "} // namespace strategy\n"}),
    Tool("strategy_classes", "Strategy classes",
         STRATEGY + "The strategy classes there are, each with its parameters (default, min, max, integer), the file "
         "it is written in and whether it is a demo one; and the strategy libraries that could not be loaded, "
         "with why.",
         NoInput, strategy_classes, "/backtest/strategies", method="GET", engine=False, record=False),
    Tool("build_strategies", "Build the strategies",
         STRATEGY + "Builds every strategy library again (demo, research, paper and live): "
         "after the engine changed, a library built against its old headers is refused until then. Refused with the "
         "compiler's errors when one does not build; else the classes, as strategy_classes lists them.",
         NoInput, build_strategies, "/backtest/strategies/build", cost="seconds", engine=False, record=False),
    Tool("install_strategy", "Install a strategy for an account",
         STRATEGY + "Puts strategy/research/<name>.cpp, as it is now, where an account's engine loads strategies from "
         "(strategy/paper or strategy/live), on a person's word, recorded in the ledger. An agent's strategy goes "
         "through "
         "promote_strategy_version instead.",
         InstallRequest, install_strategy, "/backtest/strategies/install", cost="seconds", engine=False, record=False,
         person=True, example={"name": "example_quiet", "account": "paper", "approved": "the owner: the example"}),
    Tool("uninstall_strategy", "Take a strategy off an account",
         STRATEGY + "Takes strategy/<account>/<name>.cpp out, with the reason, recorded in the ledger: the engine "
         "loads it no more once restarted.",
         UninstallRequest, uninstall_strategy, "/backtest/strategies/uninstall", engine=False, record=False,
         person=True, example={"name": "example_quiet", "account": "paper", "reason": "the example"}),
]
