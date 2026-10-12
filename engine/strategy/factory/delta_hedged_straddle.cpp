#include "delta_hedged_straddle.hpp"
#include "strategy_registry.hpp"

#include <array>
#include <format>

namespace strategy {

namespace {

// name, default, min, max, integer
constexpr std::array kParameters{
    StrategyParam{"position_size", 10, 1, 100, true}, // straddles
    StrategyParam{"side", -1, -1, 1, true},           // 1 buys the straddle, -1 sells it
    StrategyParam{"min_dte", 1, 0, 60, true},         // the nearest chain in [min_dte, max_dte]
    StrategyParam{"max_dte", 7, 0, 60, true},
    StrategyParam{"hedge", 1, 0, 1, true},           // 0: hold it unhedged
    StrategyParam{"hedge_timer", 1, 1, 390, true},   // hedge every this many bars
    StrategyParam{"delta_range", 20, 0, 1000, true}, // shares either side of zero left alone
};

} // namespace

DeltaHedgedStraddleStrategy::DeltaHedgedStraddleStrategy(
    StrategyContext* context, const std::string& strategy_name, const std::string& portfolio_name,
    const std::unordered_map<std::string, double>& setting)
    : OptionStrategyTemplate(context, strategy_name, portfolio_name, setting),
      position_size_(param<int>("position_size")),
      side_(param<int>("side") >= 0 ? model::Direction::LONG : model::Direction::SHORT),
      min_dte_(param<int>("min_dte")), max_dte_(param<int>("max_dte")),
      hedge_(param<int>("hedge") != 0), hedge_timer_(param<int>("hedge_timer")),
      delta_range_(param<int>("delta_range")) {}

auto DeltaHedgedStraddleStrategy::parameters() -> std::span<const StrategyParam> {
    return kParameters;
}

void DeltaHedgedStraddleStrategy::on_init_logic() {
    const auto chains = portfolio()->get_chain_by_expiry(min_dte_, max_dte_);
    if (chains.empty()) {
        set_error(std::format("No chain {} to {} days out", min_dte_, max_dte_));
        return;
    }
    chain_ = chains.front();
    subscribe_chains(std::span(&chain_, 1));
    if (hedge_) {
        register_hedging(hedge_timer_, 0, delta_range_);
    }
    write_log(std::format(
        "DeltaHedgedStraddle on {} ({}, hedge {})", chain_,
        side_ == model::Direction::LONG ? "long" : "short",
        hedge_ ? std::format("every {} bars within {} shares", hedge_timer_, delta_range_)
               : std::string("off")));
}

void DeltaHedgedStraddleStrategy::on_stop_logic() { write_log("DeltaHedgedStraddle stopped"); }

void DeltaHedgedStraddleStrategy::on_timer_logic() {
    if (entered_ || error() || has_active_order()) {
        return;
    }
    const model::ChainData* chain = get_chain(chain_);
    if (chain == nullptr || chain->atm_strike <= 0) {
        return;
    }
    const auto call = chain->calls.find(chain->atm_strike);
    const auto put = chain->puts.find(chain->atm_strike);
    if (call == chain->calls.end() || put == chain->puts.end() || call->second->mid_price <= 0.0 ||
        put->second->mid_price <= 0.0) {
        return;
    }
    const auto ids =
        option_order(model::ComboType::STRADDLE, {{"call", call->second}, {"put", put->second}},
                     side_, 0.0, position_size_, model::OrderType::MARKET);
    if (!ids.empty()) {
        entered_ = true;
        write_log(std::format("Entered {} {} ATM straddles at strike {}",
                              side_ == model::Direction::LONG ? "long" : "short",
                              position_size_, chain->atm_strike));
    }
}

REGISTER_STRATEGY(DeltaHedgedStraddleStrategy);

} // namespace strategy
