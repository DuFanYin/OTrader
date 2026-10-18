/** Portfolios: the portfolios, their contracts loaded and rolled, a contract's own. */

#include "model/portfolios.hpp"
#include "model/portfolio_spec.hpp"
#include "model/symbol.hpp"
#include <algorithm>
#include <map>

namespace model {

void Portfolios::ensure_portfolios_created() {
    for (const PortfolioSpec& p : kPortfolios) {
        const std::string name(p.name);
        ensure_portfolio(name);
        Portfolio& port = *find_portfolio(name);
        port.underlying_symbol = p.underlying;
        port.physical = p.physical;
        if (p.hedge != p.underlying) {
            port.hedge_symbol = p.hedge;
        }
    }
}

void Portfolios::ensure_portfolio(const std::string& portfolio_name) {
    if (!portfolios_.contains(portfolio_name)) {
        portfolios_.emplace(portfolio_name, std::make_unique<Portfolio>(portfolio_name));
    }
}

void Portfolios::clear() { portfolios_.clear(); }

void Portfolios::load_contracts(std::span<const Contract> contracts) {
    std::map<std::string, std::vector<Contract>> options; // by root
    for (const Contract& contract : contracts) {
        if (const std::optional<OptionId> id = parse_option_symbol(contract.symbol)) {
            options[id->root].push_back(contract);
        } else {
            load_underlying(contract);
        }
    }
    for (auto& [root, added] : options) {
        if (Portfolio* port = find_portfolio(root)) {
            std::vector<Contract> listed(port->listing().contracts().begin(),
                                         port->listing().contracts().end());
            listed.append_range(added);
            port->list(std::make_shared<const Listing>(std::move(listed)));
        }
    }
}

auto Portfolios::replace_contracts(const std::string& portfolio_name,
                                   std::span<const Contract> contracts,
                                   const std::unordered_set<std::string>& keep)
    -> std::vector<std::string> {
    Portfolio* port = find_portfolio(portfolio_name);
    if (port == nullptr) {
        return {};
    }
    std::vector<Contract> options;
    for (const Contract& contract : contracts) {
        if (parse_option_symbol(contract.symbol)) {
            options.push_back(contract);
        } else {
            load_underlying(contract);
        }
    }
    return port->replace_options(options, keep);
}

auto Portfolios::replace_contracts(const std::string& portfolio_name, const Contract& underlying,
                                   std::shared_ptr<const Listing> listing)
    -> std::vector<std::string> {
    Portfolio* port = find_portfolio(portfolio_name);
    if (port == nullptr) {
        return {};
    }
    load_underlying(underlying);
    return port->list(std::move(listing));
}

auto Portfolios::find_portfolio(const std::string& portfolio_name) -> Portfolio* {
    const auto it = portfolios_.find(portfolio_name);
    return it != portfolios_.end() ? it->second.get() : nullptr;
}

auto Portfolios::find_portfolio(const std::string& portfolio_name) const
    -> const Portfolio* {
    const auto it = portfolios_.find(portfolio_name);
    return it != portfolios_.end() ? it->second.get() : nullptr;
}

auto Portfolios::portfolio_names() const -> std::vector<std::string> {
    std::vector<std::string> out;
    out.reserve(portfolios_.size());
    for (const auto& [name, _] : portfolios_) {
        out.push_back(name);
    }
    return out;
}

auto Portfolios::trades_contract(std::string_view symbol) const -> bool {
    if (const auto id = parse_option_symbol(symbol)) {
        return portfolios_.contains(std::string(id->root));
    }
    return std::ranges::any_of(portfolios_, [&](const auto& entry) {
        const Portfolio& p = *entry.second;
        return p.underlying_symbol == symbol || p.hedge_symbol == symbol;
    });
}

auto Portfolios::portfolio_of(std::string_view symbol) const -> const Portfolio* {
    if (const auto id = parse_option_symbol(symbol)) {
        return find_portfolio(id->root);
    }
    // Its own portfolio before one it hedges (SPY: SPY's underlying, SPXW's hedge), whatever the
    // map's order.
    for (const auto& [_, p] : portfolios_) {
        if (p->underlying_symbol == symbol) {
            return p.get();
        }
    }
    for (const auto& [_, p] : portfolios_) {
        if (p->hedge_symbol == symbol) {
            return p.get();
        }
    }
    return nullptr;
}

auto Portfolios::find_contract(std::string_view symbol) const -> const Contract* {
    if (parse_option_symbol(symbol)) {
        const Portfolio* port = portfolio_of(symbol);
        return port != nullptr ? port->contract(symbol) : nullptr;
    }
    for (const auto& [_, p] : portfolios_) {
        if (const Contract* contract = p->contract(symbol)) {
            return contract;
        }
    }
    return nullptr;
}

void Portfolios::load_underlying(const Contract& contract) {
    for (const PortfolioSpec& spec : kPortfolios) {
        Portfolio* port = find_portfolio(std::string(spec.name));
        if (port != nullptr && spec.hedge == contract.symbol && spec.hedge != spec.underlying) {
            port->set_hedge_instrument(contract);
        }
    }
    const auto spec = std::ranges::find(kPortfolios, contract.symbol, &PortfolioSpec::underlying);
    if (Portfolio* port =
            find_portfolio(spec != kPortfolios.end() ? std::string(spec->name) : contract.symbol)) {
        port->set_underlying(contract);
    }
}

} // namespace model
