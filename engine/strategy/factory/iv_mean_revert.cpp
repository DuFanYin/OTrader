#include "iv_mean_revert.hpp"
#include "strategy_registry.hpp"

#include <array>
#include <cmath>
#include <utility>

namespace strategy {

namespace {

// name, default, min, max, integer
constexpr std::array kParameters{
    StrategyParam{"iv_threshold_up", 2.0, 0.0, 10.0},
    StrategyParam{"iv_threshold_down", -2.0, -10.0, 0.0},
    StrategyParam{"lookback_minutes", 60, 2, 390, true},
    StrategyParam{"position_size", 1, 1, 100, true},
    StrategyParam{"min_dte", 0, 0, 60, true},
    StrategyParam{"max_dte", 3, 0, 60, true},
    StrategyParam{"exit_z", 0.5, 0.0, 10.0},
    StrategyParam{"warmup_bars", 20, 0, 390, true},
};

} // namespace

IvMeanRevertStrategy::IvMeanRevertStrategy(StrategyContext* context,
                                           const std::string& strategy_name,
                                           const std::string& portfolio_name,
                                           const std::unordered_map<std::string, double>& setting)
    : OptionStrategyTemplate(context, strategy_name, portfolio_name, setting),
      iv_threshold_up_(param("iv_threshold_up")), iv_threshold_down_(param("iv_threshold_down")),
      lookback_minutes_(param<int>("lookback_minutes")),
      position_size_(param<int>("position_size")), min_dte_(param<int>("min_dte")),
      max_dte_(param<int>("max_dte")), exit_z_(param("exit_z")),
      warmup_bars_(param<int>("warmup_bars")) {}

auto IvMeanRevertStrategy::parameters() -> std::span<const StrategyParam> { return kParameters; }

void IvMeanRevertStrategy::on_init_logic() {
    // The nearest chain in [min_dte, max_dte]
    auto chains = portfolio()->get_chain_by_expiry(min_dte_, max_dte_);
    if (chains.empty()) {
        set_error("IvMeanRevert: no chains found in DTE range");
        return;
    }
    chain_symbols_ = {chains.front()};
    subscribe_chains(chain_symbols_);

    iv_history_.clear();
    last_z_ = 0.0;

    write_log("IvMeanRevert initialized on chain: " + chains.front());
}

void IvMeanRevertStrategy::on_stop_logic() {
    close_all_strategy_positions();
    write_log("IvMeanRevert stopped");
}

void IvMeanRevertStrategy::on_timer_logic() {
    if (error()) {
        return;
    }

    auto* h = holding();
    if (h == nullptr) {
        return;
    }
    if (chain_symbols_.empty()) {
        return;
    }

    auto* chain = get_chain(chain_symbols_.front());
    if (chain == nullptr) {
        return;
    }

    // ATM IV from apply_frame
    auto atm_iv_opt = chain->get_atm_iv();
    if (!atm_iv_opt.has_value()) {
        return;
    }
    double atm_iv = *atm_iv_opt;

    iv_history_.push_back(atm_iv);
    if (std::cmp_greater(iv_history_.size(), lookback_minutes_)) {
        iv_history_.erase(iv_history_.begin());
    }

    int min_samples = warmup_bars_ > 2 ? warmup_bars_ : 2;
    if (std::cmp_less(iv_history_.size(), min_samples)) {
        return;
    }

    // Mean, std, z-score
    double sum = 0.0;
    for (double v : iv_history_) {
        sum += v;
    }
    double mean = sum / static_cast<double>(iv_history_.size());

    double var = 0.0;
    for (double v : iv_history_) {
        double d = v - mean;
        var += d * d;
    }
    int n = static_cast<int>(iv_history_.size());
    if (n > 1) {
        var /= static_cast<double>(n - 1);
    }
    double stddev = std::sqrt(var);
    if (stddev <= 1e-8) {
        return;
    }

    double z = (atm_iv - mean) / stddev;
    last_z_ = z;

    // Check position
    bool has_position = false;
    for (const auto& [_, pos] : h->optionPositions) {
        if (pos.quantity != 0) {
            has_position = true;
            break;
        }
    }

    // Entry: z-score threshold
    if (!has_position) {
        // Iron butterfly for IV trade
        if (chain->atm_strike <= 0) {
            return;
        }
        // ATM index
        int atm_pos = -1;
        for (int i = 0; i < static_cast<int>(chain->strikes.size()); ++i) {
            if (chain->strikes[static_cast<size_t>(i)] == chain->atm_strike) {
                atm_pos = i;
                break;
            }
        }
        if (atm_pos < 0) {
            return;
        }

        // Wings: 2 strike steps
        constexpr int kWingSteps = 2;
        if (atm_pos - kWingSteps < 0 ||
            atm_pos + kWingSteps >= static_cast<int>(chain->strikes.size())) {
            return;
        }

        const double lower_idx = chain->strikes[atm_pos - kWingSteps];
        const double atm_idx = chain->strikes[atm_pos];
        const double upper_idx = chain->strikes[atm_pos + kWingSteps];

        // 4 legs: K-W put, K put, K call, K+W call
        auto pl_it = chain->puts.find(lower_idx);
        auto pu_it = chain->puts.find(atm_idx);
        auto cl_it = chain->calls.find(atm_idx);
        auto cu_it = chain->calls.find(upper_idx);
        if (pl_it == chain->puts.end() || pu_it == chain->puts.end() ||
            cl_it == chain->calls.end() || cu_it == chain->calls.end()) {
            return;
        }
        auto* put_lower = pl_it->second;
        auto* put_body = pu_it->second;
        auto* call_body = cl_it->second;
        auto* call_upper = cu_it->second;
        if ((put_lower == nullptr) || (put_body == nullptr) || (call_body == nullptr) ||
            (call_upper == nullptr)) {
            return;
        }
        if (put_lower->mid_price <= 0.0 || put_body->mid_price <= 0.0 ||
            call_body->mid_price <= 0.0 || call_upper->mid_price <= 0.0) {
            return;
        }

        // IRON_CONDOR (K-W, K, K, K+W) = iron fly
        std::unordered_map<std::string, const model::OptionData*> iron_data;
        iron_data["put_lower"] = put_lower;
        iron_data["put_upper"] = put_body;
        iron_data["call_lower"] = call_body;
        iron_data["call_upper"] = call_upper;

        if (z >= iv_threshold_up_) {
            // IV high → short iron fly
            auto ids = option_order(
                model::ComboType::IRON_CONDOR, iron_data, model::Direction::SHORT, 0.0,
                static_cast<double>(position_size_), model::OrderType::MARKET);
            if (!ids.empty()) {
                write_log("Enter SHORT IV mean-revert IronFly, z=" + std::to_string(z) +
                          ", atm_iv=" + std::to_string(atm_iv));
            }
        } else if (z <= iv_threshold_down_) {
            // IV low → long iron fly
            auto ids = option_order(
                model::ComboType::IRON_CONDOR, iron_data, model::Direction::LONG, 0.0,
                static_cast<double>(position_size_), model::OrderType::MARKET);
            if (!ids.empty()) {
                write_log("Enter LONG IV mean-revert IronFly, z=" + std::to_string(z) +
                          ", atm_iv=" + std::to_string(atm_iv));
            }
        }
        return;
    }

    // Exit: z-score reversion
    if (std::fabs(z) <= exit_z_) {
        close_all_strategy_positions();
        write_log("Exit IV mean-revert position, z=" + std::to_string(z) +
                  ", atm_iv=" + std::to_string(atm_iv));
    }
}

REGISTER_STRATEGY(IvMeanRevertStrategy);

} // namespace strategy
