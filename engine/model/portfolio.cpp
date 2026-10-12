#include "portfolio.hpp"
#include "symbol.hpp"

#include "../quant/kernel.h"
#include "../utilities/thread_pool.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <math.h>
#include <ranges>
#include <sstream>
#include <thread>

namespace model {

namespace {

/** The underlying's price a snapshot is priced at: its mark from its quotes, else as marked. */
auto snapshot_spot(const PortfolioSnapshot& snapshot) -> double {
    return mark(snapshot.underlying_bid, snapshot.underlying_ask, snapshot.underlying_mark);
}

/** The price IV is solved from (pick_iv_input_price, its mode read once). */
enum class IvInput : std::uint8_t { Mid, Bid, Ask };

auto iv_input(const std::string& mode) -> IvInput {
    if (mode == "bid") {
        return IvInput::Bid;
    }
    return mode == "ask" ? IvInput::Ask : IvInput::Mid;
}

auto iv_input_price(double bid, double ask, IvInput input) -> double {
    switch (input) {
    case IvInput::Bid:
        return bid > 0.0 ? bid : 0.0;
    case IvInput::Ask:
        return ask > 0.0 ? ask : 0.0;
    case IvInput::Mid:
        break;
    }
    return mark(bid, ask);
}

} // namespace

OptionData::OptionData(const ContractData& contract, const OptionId& id)
    : symbol(contract.symbol), size(contract.size), strike(id.strike), type(id.type),
      expiry(option_expiry_time(id.expiry)) {}

void OptionData::set_portfolio(PortfolioData* p) { portfolio = p; }
void OptionData::set_chain(ChainData* c) { chain = c; }
void OptionData::set_underlying(UnderlyingData* u) { underlying = u; }

UnderlyingData::UnderlyingData(const ContractData& contract)
    : symbol(contract.symbol), size(contract.size), theo_delta(contract.size) {}

void UnderlyingData::set_portfolio(PortfolioData* p) { portfolio = p; }

void UnderlyingData::add_chain(ChainData* chain) { chains[chain->chain_symbol] = chain; }

ChainData::ChainData(std::string chain_symbol_) : chain_symbol(std::move(chain_symbol_)) {}

void ChainData::add_option(OptionData* option) {
    options[option->symbol] = option;
    (option->type == OptionType::CALL ? calls : puts)[option->strike] = option;
    strikes.push_back(option->strike);
    option->set_chain(this);
    if (portfolio != nullptr) {
        count_days_to_expiry(portfolio->dte_ref());
    }
}

void ChainData::count_days_to_expiry(DateTime ref) {
    // A chain's options share one expiry.
    for (const auto& [_, option] : options) {
        if (option == nullptr) {
            continue;
        }
        auto diff_hours =
            std::chrono::duration_cast<std::chrono::hours>(option->expiry - ref).count();
        days_to_expiry = diff_hours > 0 ? static_cast<int>(diff_hours / 24) : 0;
        time_to_expiry = static_cast<double>(days_to_expiry) / ANNUAL_DAYS;
        return;
    }
}

void ChainData::sort_strikes() {
    std::ranges::sort(strikes);
    strikes.erase(std::ranges::unique(strikes).begin(), strikes.end());
}

void ChainData::set_underlying(UnderlyingData* u) {
    u->add_chain(this);
    underlying = u;
    for (auto& [_, opt] : options) {
        opt->set_underlying(u);
    }
}

void ChainData::set_portfolio(PortfolioData* p) { portfolio = p; }

void ChainData::calculate_atm_price() {
    if (strikes.empty()) {
        atm_strike = 0;
        return;
    }
    const double reference = reference_price();
    if (reference <= 0) {
        atm_strike = strikes[strikes.size() / 2];
        return;
    }
    // Ascending: of two as near, the lower.
    atm_strike = *std::ranges::min_element(strikes, {}, [reference](double strike) {
        return std::abs(strike - reference);
    });
}

auto ChainData::get_atm_iv() const -> std::optional<double> {
    if (atm_strike <= 0) {
        return std::nullopt;
    }
    auto cit = calls.find(atm_strike);
    if (cit != calls.end() && cit->second->mid_iv != 0) {
        return cit->second->mid_iv;
    }
    auto pit = puts.find(atm_strike);
    if (pit != puts.end() && pit->second->mid_iv != 0) {
        return pit->second->mid_iv;
    }
    return std::nullopt;
}

auto ChainData::reference_price() const -> double {
    if (forward > 0.0) {
        return forward;
    }
    return underlying != nullptr ? underlying->mid_price : 0.0;
}

PortfolioData::PortfolioData(std::string name_) : name(std::move(name_)) {}

void PortfolioData::set_risk_free_rate(double rate) {
    if (std::isfinite(rate)) {
        risk_free_rate_ = rate;
    }
}

void PortfolioData::set_iv_price_mode(std::string mode) {
    std::ranges::transform(mode, mode.begin(), [](unsigned char c) -> char {
        return static_cast<char>(std::tolower(c));
    });
    if (mode == "mid" || mode == "bid" || mode == "ask") {
        iv_price_mode_ = std::move(mode);
    }
}

void PortfolioData::set_dte_ref(DateTime ref) {
    dte_ref_ = ref;
    for (auto& [_, chain] : chains) {
        if (chain) {
            chain->count_days_to_expiry(ref);
        }
    }
}

void PortfolioData::apply_frame(const PortfolioSnapshot& snapshot) {
    if (snapshot.datetime != DateTime{}) {
        datetime_ = snapshot.datetime;
        // DTE counts from the snapshot's UTC date, as a backtest day counts from its own date, so
        // a live process that runs across days moves on with the calendar.
        const DateTime day = std::chrono::floor<std::chrono::days>(snapshot.datetime);
        if (day != dte_ref_) {
            set_dte_ref(day);
        }
    }
    if (underlying) {
        underlying->bid_price = snapshot.underlying_bid;
        underlying->ask_price = snapshot.underlying_ask;
        underlying->mid_price = snapshot.underlying_mark;
    }
    const size_t n = option_apply_order_.size();
    if (n != snapshot.bid.size()) {
        return;
    }
    const double spot = snapshot_spot(snapshot);
    if (hedge_instrument) {
        UnderlyingData& h = *hedge_instrument;
        h.bid_price = snapshot.hedge_bid;
        h.ask_price = snapshot.hedge_ask;
        h.mid_price = snapshot.hedge_mark;
        h.theo_delta = (h.mid_price > 0.0 && spot > 0.0) ? h.mid_price * h.size / spot : 0.0;
    }

    for (size_t i = 0; i < n; ++i) {
        OptionData* opt = option_apply_order_[i];
        if (opt == nullptr) {
            continue;
        }
        const double bid = snapshot.bid[i];
        const double ask = snapshot.ask[i];
        opt->bid_price = bid;
        opt->ask_price = ask;
        opt->mid_price = mark(bid, ask, snapshot.mark[i]);
    }

    // The pricing: done already (the market data thread, a backtest before its run), or here.
    const PortfolioSnapshot* priced = &snapshot;
    if (!snapshot.priced) {
        unpriced_ = snapshot;
        price_snapshot(unpriced_);
        priced = &unpriced_;
    }
    if (!priced->priced) {
        return; // not this portfolio's layout
    }
    for (size_t c = 0; c < pricing_chains_.size(); ++c) {
        pricing_chains_[c]->forward = priced->forward[c];
    }
    for (size_t i = 0; i < n; ++i) {
        OptionData* opt = option_apply_order_[i];
        if (opt == nullptr) {
            continue;
        }
        const double sz = opt->size != 0.0 ? opt->size : 1.0;
        opt->delta = priced->delta[i] * sz;
        opt->gamma = priced->gamma[i] * sz;
        opt->theta = priced->theta[i] * sz;
        opt->vega = priced->vega[i] * sz;
        opt->mid_iv = priced->iv[i];
    }
    for (auto& [_, chain] : chains) {
        if (chain) {
            chain->calculate_atm_price();
        }
    }
}

auto PortfolioData::chain_forward(std::size_t chain, const PortfolioSnapshot& snapshot,
                                  double spot) const -> double {
    const int expiry = pricing_chain_expiry_[chain];
    if (spot <= 0.0 || expiry < 0) {
        return 0.0;
    }
    const double t = snapshot.expiry_years[static_cast<size_t>(expiry)];
    if (t <= 0.0) {
        return 0.0;
    }
    // Put-call parity at a strike quoted both ways: C - P = e^(-rt) (F - K).
    struct Estimate {
        double gap; // |C - P|: smallest at the strikes nearest the forward
        double forward;
    };
    thread_local std::vector<Estimate> estimates;
    thread_local std::vector<double> forwards;
    estimates.clear();
    const double grow = std::exp(risk_free_rate_ * t);
    for (const auto& [call, put] : pricing_pairs_[chain]) {
        const auto c = static_cast<size_t>(call);
        const auto p = static_cast<size_t>(put);
        if (!(snapshot.bid[c] > 0.0 && snapshot.ask[c] > 0.0 && snapshot.bid[p] > 0.0 &&
              snapshot.ask[p] > 0.0)) {
            continue;
        }
        const double gap = mark(snapshot.bid[c], snapshot.ask[c]) - mark(snapshot.bid[p], snapshot.ask[p]);
        estimates.push_back({std::abs(gap), pricing_strike_[c] + (grow * gap)});
    }
    std::optional<double> implied;
    if (!estimates.empty()) {
        // The median of the (up to) three nearest: one stale quote does not move it.
        const auto nearest = std::min<std::ptrdiff_t>(3, std::ssize(estimates));
        std::ranges::partial_sort(estimates, estimates.begin() + nearest, {}, &Estimate::gap);
        forwards.clear();
        for (const Estimate& e : estimates | std::views::take(nearest)) {
            forwards.push_back(e.forward);
        }
        std::ranges::sort(forwards);
        const size_t mid = forwards.size() / 2;
        implied = forwards.size() % 2 == 1 ? forwards[mid] : 0.5 * (forwards[mid - 1] + forwards[mid]);
    }
    return implied && std::abs((*implied / spot) - 1.0) < ChainData::kMaxForwardGap
               ? *implied
               : spot * std::exp(risk_free_rate_ * t);
}

void PortfolioData::price_snapshot(PortfolioSnapshot& snapshot,
                                   std::span<const std::uint8_t> quotes_changed,
                                   bool underlying_changed) const {
    const size_t n = pricing_strike_.size();
    if (n != option_apply_order_.size() || snapshot.bid.size() != n || snapshot.ask.size() != n ||
        snapshot.mark.size() != n) {
        snapshot.priced = false;
        return;
    }
    const size_t expiries = pricing_expiries_.size();
    const size_t chain_count = pricing_chains_.size();
    const double spot = snapshot_spot(snapshot);
    // Priced again from what moved: given what changed, on a snapshot priced before.
    const bool again_only = !quotes_changed.empty() && quotes_changed.size() == n && snapshot.priced &&
                            spot > 0.0 && snapshot.iv.size() == n && snapshot.delta.size() == n &&
                            snapshot.gamma.size() == n && snapshot.theta.size() == n &&
                            snapshot.vega.size() == n && snapshot.forward.size() == chain_count &&
                            snapshot.expiry_years.size() == expiries;
    if (!again_only) {
        for (std::vector<double>* v : {&snapshot.iv, &snapshot.delta, &snapshot.gamma,
                                       &snapshot.theta, &snapshot.vega}) {
            v->assign(n, 0.0);
        }
        snapshot.forward.assign(chain_count, 0.0);
        snapshot.expiry_years.assign(expiries, 0.0);
    }

    thread_local std::vector<std::uint8_t> expiry_moved;
    thread_local std::vector<std::uint8_t> chain_moved;
    expiry_moved.assign(expiries, 1);
    chain_moved.assign(chain_count, 1);
    for (size_t e = 0; e < expiries; ++e) {
        const double t = years_to_expiry(snapshot.datetime, pricing_expiries_[e]);
        expiry_moved[e] = static_cast<std::uint8_t>(!again_only || t != snapshot.expiry_years[e]);
        snapshot.expiry_years[e] = t;
    }
    for (size_t c = 0; c < chain_count; ++c) {
        const double f = chain_forward(c, snapshot, spot);
        chain_moved[c] = static_cast<std::uint8_t>(!again_only || f != snapshot.forward[c]);
        snapshot.forward[c] = f;
    }

    const IvInput input = iv_input(iv_price_mode_);
    const double r = risk_free_rate_;
    // The pool's threads run `price`: they see this thread's flags through these, never their own
    // (empty) thread_local copies.
    const std::span<const std::uint8_t> expiry_flags = expiry_moved;
    const std::span<const std::uint8_t> chain_flags = chain_moved;
    auto price = [&](size_t i) {
        const int e = pricing_expiry_[i];
        if (e < 0) {
            return; // no option in this column
        }
        const int c = pricing_chain_[i];
        // Its own forward: the chain's, else spot grown (which moves with the underlying).
        const bool own_forward = c < 0 || snapshot.forward[static_cast<size_t>(c)] <= 0.0;
        const bool again = !again_only || quotes_changed[i] != 0 ||
                           expiry_flags[static_cast<size_t>(e)] != 0 ||
                           (c >= 0 && chain_flags[static_cast<size_t>(c)] != 0) ||
                           (own_forward && underlying_changed);
        if (!again && !underlying_changed) {
            return; // nothing it is priced from moved
        }
        const double k = pricing_strike_[i];
        const double t = snapshot.expiry_years[static_cast<size_t>(e)];
        const bool is_call = pricing_is_call_[i] != 0;
        const double px = iv_input_price(snapshot.bid[i], snapshot.ask[i], input);
        if (spot <= 0.0 || k <= 0.0 || t <= 0.0 || px <= 0.0) {
            snapshot.iv[i] = snapshot.delta[i] = snapshot.gamma[i] = snapshot.theta[i] =
                snapshot.vega[i] = 0.0;
            return;
        }
        const double forward = own_forward ? spot * std::exp(r * t)
                                           : snapshot.forward[static_cast<size_t>(c)];
        const double iv = again ? quant::black_implied_vol(px, forward, k, t, r, is_call)
                                : snapshot.iv[i];
        const quant::BlackGreeks g = quant::black_greeks(is_call, spot, forward, k, t, r, iv);
        snapshot.iv[i] = iv;
        snapshot.delta[i] = g.delta;
        snapshot.gamma[i] = g.gamma;
        snapshot.theta[i] = g.theta;
        snapshot.vega[i] = g.vega;
    };
    if (!again_only || underlying_changed) {
        // Many to price: data-parallel and independent, on the shared pool (a small chain, or a
        // pool busy with another thread's range, inline).
        utilities::shared_pool().parallel_for(0, n, price);
    } else {
        for (size_t i = 0; i < n; ++i) {
            price(i);
        }
    }
    snapshot.priced = true;
}

void PortfolioData::set_underlying(const ContractData& contract) {
    underlying = std::make_unique<UnderlyingData>(contract);
    underlying->set_portfolio(this);
    underlying_symbol = contract.symbol;
    for (auto& [_, chain] : chains) {
        if (chain) {
            chain->set_underlying(underlying.get());
        }
    }
}

void PortfolioData::set_hedge_instrument(const ContractData& contract) {
    hedge_symbol = contract.symbol;
    hedge_instrument = std::make_unique<UnderlyingData>(contract);
    hedge_instrument->set_portfolio(this);
    hedge_instrument->theo_delta = 0.0; // until both it and the underlying have a price
}

auto PortfolioData::hedge() const -> const UnderlyingData* {
    return hedge_symbol.empty() ? underlying.get() : hedge_instrument.get();
}

auto PortfolioData::get_chain(const std::string& chain_symbol) -> ChainData* {
    auto it = chains.find(chain_symbol);
    if (it != chains.end() && (it->second != nullptr)) {
        return it->second.get();
    }
    auto chain = std::make_unique<ChainData>(chain_symbol);
    chain->set_portfolio(this);
    // If portfolio has underlying, attach to new chain
    // For calculate_atm_price
    if (underlying) {
        chain->set_underlying(underlying.get());
    }
    ChainData* ptr = chain.get();
    chains[chain_symbol] = std::move(chain);
    return ptr;
}

auto PortfolioData::get_chain_by_expiry(int min_dte, int max_dte) const
    -> std::vector<std::string> {
    auto matching = chains | std::views::filter([min_dte, max_dte](const auto& kv) {
                        return kv.second && min_dte <= kv.second->days_to_expiry &&
                               kv.second->days_to_expiry <= max_dte;
                    }) |
                    std::views::transform([](const auto& kv) { return kv.first; });
    std::vector<std::string> out;
    std::ranges::copy(matching, std::back_inserter(out));
    std::ranges::sort(out);
    return out;
}

void PortfolioData::add_option(const ContractData& contract) {
    const std::optional<OptionId> id = parse_option_symbol(contract.symbol);
    if (!id) {
        return; // not an option the platform names (the loader reports it)
    }
    auto it = options.find(contract.symbol);
    if (it == options.end()) {
        it = options.emplace(contract.symbol, OptionData(contract, *id)).first;
    } else {
        it->second = OptionData(contract, *id);
    }
    it->second.set_portfolio(this);
    get_chain(chain_symbol(id->root, id->expiry))->add_option(&it->second);
}

void PortfolioData::finalize_chains() {
    for (auto& [_, chain] : chains) {
        if (chain) {
            chain->sort_strikes();
        }
    }
    option_apply_order_.clear();
    std::vector<std::string> chain_symbols;
    chain_symbols.reserve(chains.size());
    std::ranges::copy(chains | std::views::keys, std::back_inserter(chain_symbols));
    std::ranges::sort(chain_symbols);
    for (const std::string& ckey : chain_symbols) {
        auto it = chains.find(ckey);
        if (it == chains.end() || !it->second) {
            continue;
        }
        ChainData* ch = it->second.get();
        std::vector<OptionData*> opts;
        opts.reserve(ch->options.size());
        std::ranges::copy(ch->options | std::views::values, std::back_inserter(opts));
        std::ranges::sort(
            opts, [](OptionData* a, OptionData* b) -> bool { return a->symbol < b->symbol; });
        for (OptionData* opt : opts) {
            option_apply_order_.push_back(opt);
        }
    }

    lay_out_pricing();
}

void PortfolioData::lay_out_pricing() {
    // price_snapshot reads nothing else of the portfolio.
    const size_t n = option_apply_order_.size();
    pricing_strike_.assign(n, 0.0);
    pricing_is_call_.assign(n, 0);
    pricing_expiry_.assign(n, -1);
    pricing_chain_.assign(n, -1);
    pricing_expiries_.clear();
    pricing_chains_.clear();
    pricing_chain_expiry_.clear();
    pricing_pairs_.clear();
    const auto expiry_index = [this](DateTime expiry) -> int {
        const auto it = std::ranges::find(pricing_expiries_, expiry);
        if (it != pricing_expiries_.end()) {
            return static_cast<int>(it - pricing_expiries_.begin());
        }
        pricing_expiries_.push_back(expiry);
        return static_cast<int>(pricing_expiries_.size()) - 1;
    };
    std::unordered_map<const ChainData*, int> chain_index;
    for (auto& [_, chain] : chains) {
        if (chain) {
            chain_index[chain.get()] = static_cast<int>(pricing_chains_.size());
            pricing_chains_.push_back(chain.get());
        }
    }
    std::unordered_map<const OptionData*, int> column;
    for (size_t i = 0; i < n; ++i) {
        const OptionData* opt = option_apply_order_[i];
        if (opt == nullptr) {
            continue;
        }
        column[opt] = static_cast<int>(i);
        pricing_strike_[i] = opt->strike;
        pricing_is_call_[i] = static_cast<std::uint8_t>(opt->type == OptionType::CALL);
        pricing_expiry_[i] = expiry_index(opt->expiry);
        if (const auto it = chain_index.find(opt->chain); it != chain_index.end()) {
            pricing_chain_[i] = it->second;
        }
    }
    for (const ChainData* chain : pricing_chains_) {
        const OptionData* first = chain->options.empty() ? nullptr : chain->options.begin()->second;
        pricing_chain_expiry_.push_back(first != nullptr ? expiry_index(first->expiry) : -1);
        std::vector<std::pair<int, int>>& pairs = pricing_pairs_.emplace_back();
        for (const auto& [strike, call] : chain->calls) { // ascending
            const auto put = chain->puts.find(strike);
            if (put == chain->puts.end() || call == nullptr || put->second == nullptr) {
                continue;
            }
            const auto c = column.find(call);
            const auto p = column.find(put->second);
            if (c != column.end() && p != column.end()) {
                pairs.emplace_back(c->second, p->second);
            }
        }
    }
}

void PortfolioData::calculate_atm_price() {
    for (auto& [_, chain] : chains) {
        if (chain) {
            chain->calculate_atm_price();
        }
    }
}

} // namespace model
