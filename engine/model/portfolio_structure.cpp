/** PortfolioStructure: load_contracts → portfolios + contracts. */

#include "portfolio_structure.hpp"
#include "symbol.hpp"
#include "portfolios.hpp"
#include <algorithm>

namespace model {

namespace {

std::string portfolio_name_for_underlying(const std::string& ticker) {
    const auto it = std::ranges::find(kPortfolios, ticker, &PortfolioSpec::underlying);
    return it != kPortfolios.end() ? std::string(it->name) : ticker;
}

} // namespace

void PortfolioStructure::ensure_portfolios_created() {
    for (const PortfolioSpec& p : kPortfolios) {
        model::PortfolioData* port = get_or_create_portfolio(std::string(p.name));
        port->underlying_symbol = p.underlying;
        port->physical = p.physical;
        if (p.hedge != p.underlying) {
            port->hedge_symbol = p.hedge;
        }
    }
}

void PortfolioStructure::ensure_portfolio(const std::string& portfolio_name) {
    get_or_create_portfolio(portfolio_name);
}

void PortfolioStructure::clear() {
    portfolios_.clear();
    contracts_.clear();
}

void PortfolioStructure::process_option(const model::ContractData& contract) {
    process_contract(contract, true);
}

void PortfolioStructure::process_underlying(const model::ContractData& contract) {
    process_contract(contract, false);
}

void PortfolioStructure::process_option_for_portfolio(const std::string& portfolio_name,
                                                      const model::ContractData& contract) {
    contracts_[contract.symbol] = contract;
    model::PortfolioData* port = get_portfolio(portfolio_name);
    if (port != nullptr) {
        port->add_option(contract);
    }
}

void PortfolioStructure::process_underlying_for_portfolio(const std::string& portfolio_name,
                                                          const model::ContractData& contract) {
    contracts_[contract.symbol] = contract;
    model::PortfolioData* port = get_portfolio(portfolio_name);
    if (port != nullptr) {
        port->set_underlying(contract);
    }
}

void PortfolioStructure::process_contract(const model::ContractData& contract, bool is_option) {
    contracts_[contract.symbol] = contract;
    if (!is_option) {
        // An underlying is also what portfolios on an untradable one hedge with.
        for (const PortfolioSpec& spec : kPortfolios) {
            model::PortfolioData* port = get_portfolio(std::string(spec.name));
            if (port != nullptr && spec.hedge == contract.symbol && spec.hedge != spec.underlying) {
                port->set_hedge_instrument(contract);
            }
        }
    }
    // An option belongs to the portfolio its root names; an underlying, to the one on it.
    std::string portfolio_name;
    if (!is_option) {
        portfolio_name = portfolio_name_for_underlying(contract.symbol);
    } else if (const auto id = model::parse_option_symbol(contract.symbol)) {
        portfolio_name = id->root;
    }
    model::PortfolioData* port = get_portfolio(portfolio_name);
    if (port == nullptr) {
        return;
    }
    if (is_option) {
        port->add_option(contract);
    } else {
        port->set_underlying(contract);
    }
}

void PortfolioStructure::finalize_all_chains() {
    for (auto& kv : portfolios_) {
        if (kv.second) {
            kv.second->finalize_chains();
        }
    }
}

model::PortfolioData*
PortfolioStructure::get_or_create_portfolio(const std::string& portfolio_name) {
    auto it = portfolios_.find(portfolio_name);
    if (it != portfolios_.end()) {
        return it->second.get();
    }
    portfolios_[portfolio_name] = std::make_unique<model::PortfolioData>(portfolio_name);
    return portfolios_[portfolio_name].get();
}

model::PortfolioData* PortfolioStructure::get_portfolio(const std::string& portfolio_name) {
    auto it = portfolios_.find(portfolio_name);
    return (it != portfolios_.end()) ? it->second.get() : nullptr;
}

const model::PortfolioData*
PortfolioStructure::get_portfolio(const std::string& portfolio_name) const {
    auto it = portfolios_.find(portfolio_name);
    return (it != portfolios_.end()) ? it->second.get() : nullptr;
}

std::vector<std::string> PortfolioStructure::get_all_portfolio_names() const {
    std::vector<std::string> out;
    out.reserve(portfolios_.size());
    for (const auto& kv : portfolios_) {
        out.push_back(kv.first);
    }
    return out;
}

auto PortfolioStructure::trades_contract(std::string_view symbol) const -> bool {
    if (const auto id = model::parse_option_symbol(symbol)) {
        return portfolios_.contains(std::string(id->root));
    }
    return std::ranges::any_of(portfolios_, [&](const auto& kv) {
        const model::PortfolioData& p = *kv.second;
        return p.underlying_symbol == symbol || p.hedge_symbol == symbol;
    });
}

auto PortfolioStructure::portfolio_of(std::string_view symbol) const -> const model::PortfolioData* {
    if (const auto id = model::parse_option_symbol(symbol)) {
        return get_portfolio(id->root);
    }
    for (const auto& [_, p] : portfolios_) {
        if (p->underlying_symbol == symbol || p->hedge_symbol == symbol) {
            return p.get();
        }
    }
    return nullptr;
}

const model::ContractData* PortfolioStructure::get_contract(const std::string& symbol) const {
    auto it = contracts_.find(symbol);
    return (it != contracts_.end()) ? &it->second : nullptr;
}

std::vector<model::ContractData> PortfolioStructure::get_all_contracts() const {
    std::vector<model::ContractData> out;
    out.reserve(contracts_.size());
    for (const auto& kv : contracts_) {
        out.push_back(kv.second);
    }
    return out;
}

} // namespace model
