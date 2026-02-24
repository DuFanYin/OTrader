#include "portfolio.hpp"
#include <cmath>
#include <sstream>
#include <algorithm>

namespace utilities {

OptionData::OptionData(const ContractData& contract)
    : symbol(contract.symbol),
      exchange(contract.exchange),
      size(contract.size),
      strike_price(contract.option_strike),
      chain_index(contract.option_index),
      option_type(contract.option_type == OptionType::CALL ? 1 : -1),
      option_expiry(contract.option_expiry) {}

void OptionData::set_portfolio(PortfolioData* p) { portfolio = p; }
void OptionData::set_chain(ChainData* c) { chain = c; }
void OptionData::set_underlying(UnderlyingData* u) { underlying = u; }

std::optional<double> OptionData::moneyness(bool use_log) const {
    if (!underlying || !strike_price || *strike_price == 0) return std::nullopt;
    double s = underlying->mid_price;
    double k = *strike_price;
    double ratio = s / k;
    if (use_log) {
        if (ratio <= 0) return std::nullopt;
        return std::log(ratio);
    }
    return ratio;
}

bool OptionData::is_otm() const {
    if (!underlying || !strike_price) return false;
    double s = underlying->mid_price;
    double k = *strike_price;
    if (option_type > 0) return k > s;
    return k < s;
}

UnderlyingData::UnderlyingData(const ContractData& contract)
    : symbol(contract.symbol), exchange(contract.exchange), size(contract.size), theo_delta(contract.size) {}

void UnderlyingData::set_portfolio(PortfolioData* p) { portfolio = p; }

void UnderlyingData::add_chain(ChainData* chain) {
    chains[chain->chain_symbol] = chain;
}

void UnderlyingData::update_underlying_tick(const TickData& tick_data) {
    tick = tick_data;
    bid_price = tick_data.bid_price_1;
    ask_price = tick_data.ask_price_1;
    mid_price = (tick_data.bid_price_1 + tick_data.ask_price_1) / 2.0;
}

ChainData::ChainData(std::string chain_symbol_)
    : chain_symbol(std::move(chain_symbol_)) {}

void ChainData::add_option(OptionData* option) {
    options[option->symbol] = option;
    if (option->chain_index) {
        std::string idx = *option->chain_index;
        if (option->option_type > 0)
            calls[idx] = option;
        else
            puts[idx] = option;
    }
    option->set_chain(this);
    if (option->chain_index) {
        std::string idx = *option->chain_index;
        if (index_set.insert(idx).second)
            indexes.push_back(idx);
    }
    if (days_to_expiry == 0 && option->option_expiry) {
        days_to_expiry = calculate_days_to_expiry(*option->option_expiry);
        time_to_expiry = static_cast<double>(days_to_expiry) / ANNUAL_DAYS;
    }
}

void ChainData::sort_indexes() {
    if (indexes.empty()) return;
    try {
        std::vector<std::pair<double, std::string>> tmp;
        tmp.reserve(indexes.size());
        for (const auto& s : indexes)
            tmp.emplace_back(std::stod(s), s);
        std::sort(tmp.begin(), tmp.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        indexes.clear();
        for (auto& p : tmp)
            indexes.push_back(std::move(p.second));
    } catch (...) {
        std::sort(indexes.begin(), indexes.end());
    }
}

void ChainData::update_option_chain(const ChainMarketData& market_data) {
    if (underlying) underlying->mid_price = market_data.underlying_last;
    for (const auto& [sym, opt_md] : market_data.options) {
        auto it = options.find(sym);
        if (it != options.end()) {
            OptionData* opt = it->second;
            opt->bid_price = opt_md.bid_price;
            opt->ask_price = opt_md.ask_price;
            opt->mid_price = opt_md.last_price;
            opt->delta = opt_md.delta * opt->size;
            opt->gamma = opt_md.gamma * opt->size;
            opt->theta = opt_md.theta * opt->size;
            opt->vega = opt_md.vega * opt->size;
            opt->mid_iv = opt_md.mid_iv;
        }
    }
    calculate_atm_price();
}

void ChainData::set_underlying(UnderlyingData* u) {
    u->add_chain(this);
    underlying = u;
    for (auto& [_, opt] : options)
        opt->set_underlying(u);
}

void ChainData::set_portfolio(PortfolioData* p) { portfolio = p; }

void ChainData::calculate_atm_price() {
    std::vector<std::pair<double, std::string>> strike_entries;
    std::unordered_set<std::string> seen;
    for (const auto& [idx, opt] : calls) {
        if (opt->strike_price && seen.insert(idx).second)
            strike_entries.emplace_back(*opt->strike_price, idx);
    }
    for (const auto& [idx, opt] : puts) {
        if (opt->strike_price && seen.insert(idx).second)
            strike_entries.emplace_back(*opt->strike_price, idx);
    }
    if (strike_entries.empty()) {
        atm_price = 0;
        atm_index.clear();
        return;
    }
    double underlying_price = underlying ? underlying->mid_price : 0;
    double selected_strike;
    std::string selected_index;
    if (underlying_price > 0) {
        auto best = std::min_element(strike_entries.begin(), strike_entries.end(),
            [underlying_price](const auto& a, const auto& b) {
                return std::abs(a.first - underlying_price) < std::abs(b.first - underlying_price);
            });
        selected_strike = best->first;
        selected_index = best->second;
    } else {
        std::sort(strike_entries.begin(), strike_entries.end());
        size_t mid = strike_entries.size() / 2;
        selected_strike = strike_entries[mid].first;
        selected_index = strike_entries[mid].second;
    }
    atm_price = selected_strike;
    atm_index = selected_index;
}

std::optional<double> ChainData::get_atm_iv() const {
    if (atm_index.empty()) return std::nullopt;
    auto cit = calls.find(atm_index);
    if (cit != calls.end() && cit->second->mid_iv != 0) return cit->second->mid_iv;
    auto pit = puts.find(atm_index);
    if (pit != puts.end() && pit->second->mid_iv != 0) return pit->second->mid_iv;
    return std::nullopt;
}

std::optional<double> ChainData::best_iv(const std::unordered_map<std::string, OptionData*>& options_map, double target) const {
    double min_diff = 1e30;
    std::optional<double> best;
    for (const auto& [_, opt] : options_map) {
        if (opt->mid_iv == 0 || !opt->is_otm()) continue;
        double size = opt->size != 0 ? opt->size : 1.0;
        double d = opt->delta / size;
        double diff = std::abs(std::abs(d) - target);
        if (diff < min_diff) {
            min_diff = diff;
            best = opt->mid_iv;
        }
    }
    return best;
}

std::optional<double> ChainData::get_skew(double delta_target) const {
    double target = delta_target / 100.0;
    auto call_iv = best_iv(calls, target);
    auto put_iv = best_iv(puts, target);
    if (!call_iv || !put_iv || *put_iv == 0) return std::nullopt;
    return *call_iv / *put_iv;
}

PortfolioData::PortfolioData(std::string name_)
    : name(std::move(name_)) {}

void PortfolioData::update_option_chain(const ChainMarketData& market_data) {
    auto it = chains.find(market_data.chain_symbol);
    if (it != chains.end() && it->second)
        it->second->update_option_chain(market_data);
}

void PortfolioData::update_underlying_tick(const TickData& tick_data) {
    if (underlying && tick_data.symbol == underlying->symbol)
        underlying->update_underlying_tick(tick_data);
}

void PortfolioData::apply_frame(const PortfolioSnapshot& snapshot) {
    if (underlying) {
        underlying->bid_price = snapshot.underlying_bid;
        underlying->ask_price = snapshot.underlying_ask;
        underlying->mid_price = snapshot.underlying_last;
    }
    const size_t n = option_apply_order_.size();
    if (n != snapshot.bid.size()) return;
    for (size_t i = 0; i < n; ++i) {
        OptionData* opt = option_apply_order_[i];
        if (!opt) continue;
        opt->bid_price = snapshot.bid[i];
        opt->ask_price = snapshot.ask[i];
        opt->mid_price = snapshot.last[i];
        const double sz = opt->size != 0.0 ? opt->size : 1.0;
        opt->delta = snapshot.delta[i] * sz;
        opt->gamma = snapshot.gamma[i] * sz;
        opt->theta = snapshot.theta[i] * sz;
        opt->vega = snapshot.vega[i] * sz;
        opt->mid_iv = snapshot.iv[i];
    }
    for (auto& [_, chain] : chains)
        if (chain) chain->calculate_atm_price();
}

void PortfolioData::set_underlying(const ContractData& contract) {
    underlying = std::make_unique<UnderlyingData>(contract);
    underlying->set_portfolio(this);
    underlying_symbol = contract.symbol;
    for (auto& [_, chain] : chains)
        if (chain) chain->set_underlying(underlying.get());
}

ChainData* PortfolioData::get_chain(const std::string& chain_symbol) {
    auto it = chains.find(chain_symbol);
    if (it != chains.end() && it->second.get())
        return it->second.get();
    auto chain = std::make_unique<ChainData>(chain_symbol);
    chain->set_portfolio(this);
    ChainData* ptr = chain.get();
    chains[chain_symbol] = std::move(chain);
    return ptr;
}

std::vector<std::string> PortfolioData::get_chain_by_expiry(int min_dte, int max_dte) const {
    std::vector<std::string> out;
    for (const auto& [sym, chain] : chains) {
        if (chain && min_dte <= chain->days_to_expiry && chain->days_to_expiry <= max_dte)
            out.push_back(sym);
    }
    // Sort by chain symbol (e.g. SPX_20250227) so nearest expiry comes first; match Python behavior
    std::sort(out.begin(), out.end());
    return out;
}

void PortfolioData::add_option(const ContractData& contract) {
    auto it = options.find(contract.symbol);
    if (it == options.end()) {
        it = options.emplace(contract.symbol, OptionData(contract)).first;
    } else {
        it->second = OptionData(contract);
    }
    it->second.set_portfolio(this);
    OptionData* opt_ptr = &it->second;

    std::string underlying_name;
    std::string expiry_str;
    std::istringstream iss(contract.symbol);
    std::getline(iss, underlying_name, '-');
    std::getline(iss, expiry_str, '-');
    std::string chain_symbol = underlying_name + "_" + expiry_str;

    ChainData* chain = get_chain(chain_symbol);
    chain->add_option(opt_ptr);
}

void PortfolioData::finalize_chains() {
    for (auto& [_, chain] : chains)
        if (chain) chain->sort_indexes();
    option_apply_order_.clear();
    std::vector<std::string> chain_symbols;
    chain_symbols.reserve(chains.size());
    for (const auto& [sym, _] : chains)
        chain_symbols.push_back(sym);
    std::sort(chain_symbols.begin(), chain_symbols.end());
    for (const std::string& ckey : chain_symbols) {
        auto it = chains.find(ckey);
        if (it == chains.end() || !it->second) continue;
        ChainData* ch = it->second.get();
        std::vector<OptionData*> opts;
        opts.reserve(ch->options.size());
        for (const auto& [_, opt] : ch->options)
            opts.push_back(opt);
        std::sort(opts.begin(), opts.end(), [](OptionData* a, OptionData* b) { return a->symbol < b->symbol; });
        for (OptionData* opt : opts)
            option_apply_order_.push_back(opt);
    }
}

void PortfolioData::calculate_atm_price() {
    for (auto& [_, chain] : chains)
        if (chain) chain->calculate_atm_price();
}

}  // namespace utilities
